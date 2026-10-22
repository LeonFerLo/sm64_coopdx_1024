/* Partidas masivas listadas en coopnet: ver ns_smnet_lobby.h y coopnet/MASIVO.md.
 *
 * Host: con CoopNet y más de 16 jugadores, el juego arranca la partida masiva de siempre (smnet) y
 * además mantiene una conexión con el servidor coopnet: crea el lobby (el servidor lo marca como
 * masivo y manda una clave), se registra cada 5 s en el encuentro UDP desde su socket de smnet (así
 * el servidor ve su dirección pública y la cantidad de jugadores) y, cuando alguien entra, el
 * servidor le avisa y el host le manda unos paquetes para abrir su NAT.
 *
 * El que se une le pregunta al encuentro la dirección del host (ENTRAR) desde el socket del prejoin
 * y, después, desde el del juego; con eso el host perfora hacia cada uno. También mide su NAT (una
 * sonda a cada puerto del encuentro) para que el coordinador no lo haga SN si es simétrico. */
#ifdef SMNET
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ns_smnet_lobby.h"
#include "ns_smnet.h"
#include "smnet_udp.h"
#include "pc/configfile.h"
#include "pc/debuglog.h"
#include "pc/network/version.h"
#include "pc/mods/mods.h"
#include "pc/djui/djui_chat_message.h"
#ifdef COOPNET
#include "libcoopnet.h"
#endif

#define RV_MAGIC "SMRV"

/* ------------------------------------------------------------------ mensajes del encuentro */
bool ns_rv_server_addr(int which, smnet_addr_t* out) {
    char host[128]; unsigned port = configCoopNetPort ? configCoopNetPort : 34197;
    snprintf(host, sizeof host, "%s", configCoopNetIp);
    const char* e = getenv("COOPDX_COOPNET");
    if (e && e[0]) {
        snprintf(host, sizeof host, "%s", e);
        char* c = strrchr(host, ':');
        if (c) { *c = 0; port = (unsigned)atoi(c + 1); }
    }
    return smnet_addr_parse(host, (uint16_t)(port + 1 + (which ? 1 : 0)), out) == 0;
}
bool ns_rv_is_msg(const uint8_t* d, int len) { return len >= 5 && memcmp(d, RV_MAGIC, 4) == 0; }
static void hdr(uint8_t* b, uint8_t t) { memcpy(b, RV_MAGIC, 4); b[4] = t; }
int ns_rv_build_probe(uint8_t* b, uint32_t nonce) { hdr(b, RV_PROBE); memcpy(b + 5, &nonce, 4); return 9; }
int ns_rv_build_join(uint8_t* b, uint64_t lobbyId, uint32_t nonce, const char* password) {
    hdr(b, RV_JOIN); memcpy(b + 5, &lobbyId, 8); memcpy(b + 13, &nonce, 4);
    int pl = password ? (int)strlen(password) : 0; if (pl > 63) pl = 63;
    b[17] = (uint8_t)pl; if (pl) memcpy(b + 18, password, (size_t)pl);
    return 18 + pl;
}
int ns_rv_build_report(uint8_t* b, int nat) { hdr(b, RV_REPORT); b[5] = (uint8_t)nat; return 6; }
int ns_rv_nat_from_probes(uint16_t localPort, uint16_t pubA, uint16_t pubB, bool gotA, bool gotB) {
    if (!gotA || !gotB) return NS_NAT_UNKNOWN;
    if (pubA != pubB) return NS_NAT_SYMMETRIC;
    return pubA == localPort ? NS_NAT_OPEN : NS_NAT_CONE;
}
const char* ns_rv_nat_name(int nat) {
    switch (nat) { case NS_NAT_OPEN: return "abierto"; case NS_NAT_CONE: return "de cono"; case NS_NAT_SYMMETRIC: return "simétrico"; default: return "sin respuesta"; }
}
const char* ns_rv_join_error(int s) {
    switch (s) {
        case 0: return "";
        case 1: return "La partida ya no existe";
        case 2: return "No es una partida masiva";
        case 3: return "Contraseña incorrecta";
        case 4: return "El host no responde (se registra cada pocos segundos: probá de nuevo)";
        case 5: return "La partida está llena o cerrada";
        default: return "El servidor rechazó la entrada";
    }
}

/* ------------------------------------------------------------------ el que se une */
static int sNatHint = NS_NAT_UNKNOWN;
static uint64_t sJoinLobby = 0;
static char sJoinPw[64];
static double sJoinPingAt = 0;
static int sJoinPings = 0;
void ns_smnet_set_nat_hint(int nat) { sNatHint = nat; }
int ns_smnet_nat_hint(void) { return sNatHint; }
void ns_smnet_set_join_lobby(uint64_t lobbyId, const char* password) {
    sJoinLobby = lobbyId;
    snprintf(sJoinPw, sizeof sJoinPw, "%s", password ? password : "");
    sJoinPings = 0; sJoinPingAt = 0;
}
/* desde el socket del juego: los primeros 20 s, cada 2 s, hasta que smnet conecte */
void ns_smnet_lobby_join_ping(int fd, double now) {
    if (!sJoinLobby || fd < 0 || sJoinPings >= 10 || now - sJoinPingAt < 2000) return;
    smnet_addr_t a;
    if (!ns_rv_server_addr(0, &a)) return;
    uint8_t b[96];
    int n = ns_rv_build_join(b, sJoinLobby, 9000u + (uint32_t)sJoinPings, sJoinPw);
    smnet_udp_send(fd, &a, b, n);
    sJoinPingAt = now; sJoinPings++;
}

/* ------------------------------------------------------------------ host */
static bool sHostRequested = false;
#ifdef COOPNET
static struct {
    bool active, connected, created;
    uint64_t lobbyId;
    uint8_t secret[16];
    uint16_t portA, portB;
    double lastReg, probeAt, startedAt;
    uint16_t pubA, pubB; bool gotA, gotB; int nat; bool reported;
    smnet_addr_t pub;
    unsigned punches;
} sH;

static void h_on_connected(uint64_t userId) {
    (void)userId;
    sH.connected = true;
    char mode[64] = "";
    mods_get_main_mod_name(mode, sizeof mode);
    char desc[512];
    snprintf(desc, sizeof desc, "%s\n\nPartida masiva (smnet): hasta %u jugadores", get_version(), configAmountOfPlayers);
    coopnet_lobby_create(GAME_NAME, get_version(), configPlayerName, mode, (uint16_t)configAmountOfPlayers, configPassword, desc);
    LOG_INFO("smnet: lobby masivo pedido a coopnet (%u jugadores)", configAmountOfPlayers);
}
static void h_on_massive(uint64_t lobbyId, const uint8_t secret[16], uint16_t a, uint16_t b) {
    sH.lobbyId = lobbyId; memcpy(sH.secret, secret, 16); sH.portA = a; sH.portB = b; sH.created = true;
    sH.lastReg = -1e9;
    LOG_INFO("smnet: lobby masivo creado en coopnet (encuentro %u/%u)", a, b);
    djui_chat_message_create("\\#a0ffa0\\smnet: la partida aparece en la lista de CoopNet");
}
static void h_on_error(enum MPacketErrorNumber e, uint64_t tag) {
    (void)tag;
    LOG_ERROR("smnet: coopnet respondió el error %d al crear el lobby", (int)e);
}
static void h_on_disconnected(bool intentional) {
    sH.connected = false; sH.created = false;
    if (!intentional) djui_chat_message_create("\\#ffa0a0\\smnet: se cortó la conexión con CoopNet (la partida sigue, pero no aparece en la lista)");
}
#endif

void ns_smnet_lobby_host_request(void) { sHostRequested = true; }
bool ns_smnet_lobby_host_requested(void) { return sHostRequested; }

void ns_smnet_lobby_host_start(void) {
#ifdef COOPNET
    if (!sHostRequested) return;
    memset(&sH, 0, sizeof sH);
    sH.active = true; sH.nat = NS_NAT_UNKNOWN; sH.startedAt = smnet_now_ms();
    gCoopNetCallbacks.OnConnected = h_on_connected;
    gCoopNetCallbacks.OnDisconnected = h_on_disconnected;
    gCoopNetCallbacks.OnMassiveLobby = h_on_massive;
    gCoopNetCallbacks.OnError = h_on_error;
    gCoopNetCallbacks.OnLobbyCreated = NULL;
    gCoopNetCallbacks.OnLobbyJoined = NULL;
    gCoopNetCallbacks.OnLobbyLeft = NULL;
    gCoopNetCallbacks.OnReceive = NULL;
    if (coopnet_is_connected()) { h_on_connected(0); return; }
    char host[128]; unsigned port = configCoopNetPort;
    snprintf(host, sizeof host, "%s", configCoopNetIp);
    const char* e = getenv("COOPDX_COOPNET");
    if (e && e[0]) { snprintf(host, sizeof host, "%s", e); char* c = strrchr(host, ':'); if (c) { *c = 0; port = (unsigned)atoi(c + 1); } }
    uint64_t destId = strtoull(configDestId, NULL, 10);
    if (coopnet_begin(host, port, configPlayerName, destId) != COOPNET_OK) {
        djui_chat_message_create("\\#ffa0a0\\smnet: no se pudo conectar a CoopNet; la partida no va a aparecer en la lista");
        sH.active = false;
    }
#endif
}

void ns_smnet_lobby_stop(void) {
#ifdef COOPNET
    if (sH.active) {
        if (sH.created && sH.lobbyId) coopnet_lobby_leave(sH.lobbyId);
        coopnet_update();
        coopnet_shutdown();
        gCoopNetCallbacks.OnConnected = NULL; gCoopNetCallbacks.OnDisconnected = NULL;
        gCoopNetCallbacks.OnMassiveLobby = NULL; gCoopNetCallbacks.OnError = NULL;
    }
    memset(&sH, 0, sizeof sH);
#endif
    sHostRequested = false;
    sJoinLobby = 0;
}

void ns_smnet_lobby_update(int fd, int players, double now) {
#ifdef COOPNET
    if (!sH.active) return;
    coopnet_update();
    if (!sH.created || fd < 0) return;
    smnet_addr_t a;
    /* registro cada 5 s (desde el socket de smnet: así el servidor ve el mismo mapeo que usan los demás) */
    if (now - sH.lastReg >= 5000 && ns_rv_server_addr(0, &a)) {
        sH.lastReg = now;
        uint8_t b[32];
        hdr(b, RV_REG); memcpy(b + 5, &sH.lobbyId, 8); memcpy(b + 13, sH.secret, 16);
        uint16_t c = (uint16_t)(players < 1 ? 1 : players > 65535 ? 65535 : players); memcpy(b + 29, &c, 2);
        smnet_udp_send(fd, &a, b, 31);
    }
    /* tipo de NAT del host (para el log y las estadísticas) */
    if (sH.probeAt == 0 && now - sH.startedAt > 1000) {
        sH.probeAt = now;
        uint8_t b[16];
        for (int k = 0; k < 2; k++) {
            smnet_addr_t s;
            if (!ns_rv_server_addr(k, &s)) continue;
            int n = ns_rv_build_probe(b, 100u + (uint32_t)k);
            smnet_udp_send(fd, &s, b, n);
        }
    } else if (!sH.reported && sH.probeAt > 0 && now - sH.probeAt > 2000) {
        smnet_addr_t me; uint16_t lp = 0;
        if (smnet_udp_local_addr(fd, "0.0.0.0", &me) == 0) lp = smnet_addr_port(&me);
        sH.nat = ns_rv_nat_from_probes(lp, sH.pubA, sH.pubB, sH.gotA, sH.gotB);
        sH.reported = true;
        if (ns_rv_server_addr(0, &a)) { uint8_t b[8]; int n = ns_rv_build_report(b, sH.nat); smnet_udp_send(fd, &a, b, n); }
        LOG_INFO("smnet: NAT del host: %s", ns_rv_nat_name(sH.nat));
        if (sH.nat == NS_NAT_SYMMETRIC)
            djui_chat_message_create("\\#ffc080\\smnet: tu NAT es simétrico: puede que algunos no logren entrar (abrí el puerto UDP en el router)");
    }
#else
    (void)fd; (void)players; (void)now;
#endif
}

void ns_smnet_lobby_rv_rx(int fd, const smnet_addr_t* from, const uint8_t* d, int len, double now) {
    (void)from; (void)now;
    if (!ns_rv_is_msg(d, len)) return;
#ifdef COOPNET
    if (d[4] == RV_PROBE_R && len >= 16 && sH.active) {
        uint16_t p = (uint16_t)((d[13] << 8) | d[14]);
        if (d[15] == 0) { sH.pubA = p; sH.gotA = true; } else { sH.pubB = p; sH.gotB = true; }
    } else if (d[4] == RV_REG_R && len >= 12 && sH.active) {
        if (d[5] != 0) LOG_ERROR("smnet: el encuentro rechazó el registro (%d)", d[5]);
        else { sH.pub.len = 6; memcpy(sH.pub.data, d + 6, 6); }
    } else if (d[4] == RV_PUNCH && len >= 11 && sH.active && fd >= 0) {
        /* alguien entra: unos paquetes hacia él abren el NAT del host (smnet los descarta) */
        smnet_addr_t j; j.len = 6; memcpy(j.data, d + 5, 6);
        uint8_t b[8]; hdr(b, 0x7F);
        for (int k = 0; k < 3; k++) smnet_udp_send(fd, &j, b, 5);
        sH.punches++;
    }
#else
    (void)fd;
#endif
}

/* ------------------------------------------------------------------ aviso de versión nueva */

#ifdef _WIN32
__declspec(dllimport) void __stdcall Sleep(unsigned long ms);
static void rv_sleep_ms(int ms) { Sleep((unsigned long)ms); }
#else
#include <time.h>
static void rv_sleep_ms(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
#endif

bool ns_smnet_fork_update_check(char* out, int cap) {
    smnet_addr_t a;
    if (!out || cap < 2 || !ns_rv_server_addr(0, &a)) return false;
    int fd = smnet_udp_open(0);
    if (fd < 0) return false;
    uint32_t nonce = 0; smnet_random((uint8_t*)&nonce, 4);
    uint8_t b[600]; bool ok = false;
    for (int t = 0; t < 15 && !ok; t++) {
        if (t % 5 == 0) { hdr(b, RV_VERSION); memcpy(b + 5, &nonce, 4); smnet_udp_send(fd, &a, b, 9); }
        rv_sleep_ms(100);
        smnet_addr_t from; int n;
        while ((n = smnet_udp_recv(fd, &from, b, sizeof b)) >= 0) {
            if (n < 13 || !ns_rv_is_msg(b, n) || b[4] != RV_VERSION_R || memcmp(b + 5, &nonce, 4) != 0) continue;
            uint16_t build; memcpy(&build, b + 9, 2);
            int ll = b[11]; if (12 + ll + 1 > n) break;
            char label[64] = ""; snprintf(label, sizeof label, "%.*s", ll < 63 ? ll : 63, (const char*)b + 12);
            int ul = b[12 + ll]; if (13 + ll + ul > n) break;
            char url[200] = ""; snprintf(url, sizeof url, "%.*s", ul < 199 ? ul : 199, (const char*)b + 13 + ll);
            ok = true;
            if (build > SMNET_FORK_BUILD) {
                snprintf(out, (size_t)cap, "\\#ffffa0\\Hay una versión nueva: %s\n\\#dcdcdc\\Tenés la %s.%s%s",
                         label[0] ? label : "?", SMNET_FORK_LABEL, url[0] ? "\nDescargala de: " : "", url);
                smnet_udp_close(fd);
                return true;
            }
            break;
        }
    }
    smnet_udp_close(fd);
    return false;
}

/* ------------------------------------------------------------------ telemetría */

static uint32_t sTelSession = 0;
static double sFpsSum = 0; static unsigned sFpsN = 0, sFpsMin = 9999;
static int sTelEnabled = -1;

bool ns_smnet_telemetry_enabled(void) {
    /* COOPDX_SMNET_TELEMETRIA manda si está; si no, Ajustes → Partida masiva */
    if (sTelEnabled < 0) { const char* e = getenv("COOPDX_SMNET_TELEMETRIA"); sTelEnabled = (e && e[0]) ? (e[0] != '0') : 2; }
    return sTelEnabled == 2 ? configSmnetTelemetry : sTelEnabled == 1;
}
void ns_smnet_telemetry_new_session(void) {
    sTelSession = 0;
    while (sTelSession == 0) smnet_random((uint8_t*)&sTelSession, 4);
}
void ns_smnet_note_fps(unsigned fps) {
    if (fps > 255) fps = 255;
    sFpsSum += fps; sFpsN++;
    if (fps < sFpsMin) sFpsMin = fps;
}

/* [9][ver 1][sesión u32][build u16][flags][NAT][fps prom][fps mín][jugadores u16][slots][impostores u16]
   [desconexiones u16][cambios de SN u16][reentradas u16][apretones fallidos u16][retx %][rx kbps u16]
   [tx kbps u16][alojados u16][tiempo s u32] = 40 B; enteros en little endian */
void ns_smnet_telemetry_send(int fd, const ns_telem_t* t) {
    smnet_addr_t a;
    if (fd < 0 || !t || !ns_smnet_telemetry_enabled() || !ns_rv_server_addr(0, &a)) return;
    if (sTelSession == 0) ns_smnet_telemetry_new_session();
    int nat = sNatHint;
#ifdef COOPNET
    if (sH.active && sH.reported) nat = sH.nat;
#endif
    uint8_t b[64]; int p = 0;
#define P8(v)  (b[p++] = (uint8_t)(v))
#define P16(v) do { uint16_t _v = (uint16_t)(v); b[p++] = (uint8_t)_v; b[p++] = (uint8_t)(_v >> 8); } while (0)
#define P32(v) do { uint32_t _v = (uint32_t)(v); for (int _k = 0; _k < 4; _k++) b[p++] = (uint8_t)(_v >> (8 * _k)); } while (0)
    hdr(b, RV_TELEM); p = 5;
    P8(1); P32(sTelSession); P16(SMNET_FORK_BUILD); P8(t->flags); P8(nat);
    P8(sFpsN ? (unsigned)(sFpsSum / sFpsN + 0.5) : 0); P8(sFpsN ? sFpsMin : 0);
    P16(t->players); P8(t->slots); P16(t->impostors);
    P16(t->disconnects); P16(t->failovers); P16(t->rejoins); P16(t->hs_failed);
    P8(t->retx_pct); P16(t->rx_kbps); P16(t->tx_kbps); P16(t->sn_hosted); P32(t->uptime_s);
#undef P8
#undef P16
#undef P32
    smnet_udp_send(fd, &a, b, p);
    sFpsSum = 0; sFpsN = 0; sFpsMin = 9999;
}
#endif
