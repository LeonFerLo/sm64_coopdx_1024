/* Encuentro UDP para las partidas masivas (smnet), en dos puertos (A y B, por defecto 34198 y 34199).
 *
 * Todo mensaje empieza con "SMRV" y un byte de tipo. Direcciones IPv4 y puertos en orden de red.
 *
 *   SONDA      cliente -> A o B : [1][nonce u32]
 *   SONDA_R    servidor -> cliente: [2][nonce u32][ip u32][puerto u16][cuál u8: 0 = A, 1 = B]
 *       El cliente manda una sonda a cada puerto desde el mismo socket: si las dos respuestas traen
 *       el mismo puerto público, su NAT conserva el puerto (de cono: se puede perforar); si traen
 *       puertos distintos, es simétrico. Si además coincide con su puerto local, no hay NAT.
 *   REGISTRO   host -> A        : [3][partida u64][clave 16][jugadores u16]   (cada 5 s, desde su socket de smnet)
 *   REGISTRO_R servidor -> host : [4][estado u8][ip u32][puerto u16]
 *   ENTRAR     jugador -> A     : [5][partida u64][nonce u32][largo u8][contraseña]
 *   ENTRAR_R   servidor -> jug. : [6][nonce u32][estado u8][ip u32][puerto u16]   (la dirección pública del host)
 *   PERFORAR   servidor -> host : [7][ip u32][puerto u16]   (la del jugador: el host le manda unos paquetes
 *                                 para abrir su NAT; así el jugador puede entrar aunque el host no tenga
 *                                 el puerto abierto)
 *   INFORME    cliente -> A     : [8][tipo de NAT u8: 0 sin NAT, 1 de cono, 2 simétrico, 3 sin respuesta]
 *       Para las estadísticas anónimas (cuántos de cada tipo): se cuentan y se muestran cada minuto.
 *
 *   TELEMETRÍA cliente -> A    : [9][ver 1][sesión u32][build u16][flags][NAT][fps prom][fps mín][jugadores u16]
 *                                 [slots][impostores u16][desconexiones u16][cambios de SN u16][reentradas u16]
 *                                 [apretones fallidos u16][retx %][rx kbps u16][tx kbps u16][alojados u16][tiempo s u32]
 *       Anónima (ni nombres ni direcciones: la sesión es un número al azar). Cada 30 s por jugador de una
 *       masiva; se resume cada minuto en el log, en COOPNET_TELEMETRIA (JSON, por defecto telemetria.json)
 *       y en una línea por minuto de telemetria.csv. Enteros de este mensaje en little endian.
 *   VERSIÓN    cliente -> A     : [10][nonce u32]
 *   VERSIÓN_R  servidor -> cli. : [11][nonce u32][build u16 LE][largo u8][nombre][largo u8][dónde bajarla]
 *       La última versión del fork, de COOPNET_FORK_BUILD, COOPNET_FORK_VERSION y COOPNET_FORK_URL
 *       (sin COOPNET_FORK_BUILD contesta 0: el juego no avisa nada).
 *
 * Estados: 0 bien, 1 no existe la partida, 2 no es masiva, 3 contraseña o clave incorrecta,
 *          4 el host no se registró (o hace más de 30 s), 5 llena, 6 demasiados pedidos.
 */
#include "rendezvous.hpp"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <map>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include "server.hpp"
#include "logging.hpp"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#define RV_MAGIC "SMRV"

#ifdef _WIN32
#define RV_LOCALTIME(t, tm) localtime_s(tm, t)
#else
#define RV_LOCALTIME(t, tm) localtime_r(t, tm)
#endif
/* los mensajes del encuentro y de la telemetría salen siempre (LOG_ERROR solo con LOGGING=1) */
#define RV_LOG(...) do { time_t _t = time(NULL); struct tm _tm; RV_LOCALTIME(&_t, &_tm); char _b[32]; \
    strftime(_b, sizeof _b, "%Y-%m-%d %H:%M:%S", &_tm); printf("%s ", _b); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static int sSock[2] = { -1, -1 };
static uint16_t sPort[2] = { 0, 0 };
static unsigned long long sNat[4] = { 0, 0, 0, 0 };
static unsigned long long sJoins = 0, sJoinsOk = 0, sRegs = 0, sDropped = 0;

/* límites: 40 mensajes cada 10 s por dirección y puerto (un jugador manda unos pocos), y 4000 cada
   10 s por IP: con CGNAT cientos de jugadores comparten la misma IP pública y no hay que cortarlos */
struct RvRate { uint64_t window; int count; };
static std::map<uint64_t, RvRate> sRate;
static std::map<uint32_t, RvRate> sRateIp;
static bool rate_ok(uint32_t ip, uint16_t port) {
    uint64_t w = (uint64_t)time(NULL) / 10;
    if (sRate.size() > 200000) { sRate.clear(); }
    if (sRateIp.size() > 100000) { sRateIp.clear(); }
    RvRate& r = sRate[((uint64_t)ip << 16) | port];
    if (r.window != w) { r.window = w; r.count = 0; }
    RvRate& q = sRateIp[ip];
    if (q.window != w) { q.window = w; q.count = 0; }
    return ++r.count <= 40 && ++q.count <= 4000;
}

/* ------------------------------------------------------------------ telemetría */
struct Telem {
    time_t at;
    uint16_t build; uint8_t flags, nat, fps, fpsMin; uint16_t players; uint8_t slots; uint16_t imps;
    uint16_t disc, fo, rj, hsf; uint8_t retx; uint16_t rx, tx, hosted; uint32_t uptime;
};
static std::map<uint32_t, Telem> sTel;          /* la última de cada sesión */
static unsigned long long sTelDisc = 0, sTelFo = 0, sTelRj = 0, sTelHsf = 0, sTelMsgs = 0;   /* del último minuto */

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void telem_rx(const uint8_t* d, int n) {
    if (n < 40 || d[5] != 1) { return; }
    const uint8_t* p = d + 6;
    uint32_t ses = le32(p); p += 4;
    if (ses == 0) { return; }
    if (sTel.size() > 20000 && sTel.find(ses) == sTel.end()) { return; }
    Telem t;
    t.at = time(NULL);
    t.build = le16(p); p += 2; t.flags = *p++; t.nat = *p++; t.fps = *p++; t.fpsMin = *p++;
    t.players = le16(p); p += 2; t.slots = *p++; t.imps = le16(p); p += 2;
    t.disc = le16(p); p += 2; t.fo = le16(p); p += 2; t.rj = le16(p); p += 2; t.hsf = le16(p); p += 2;
    t.retx = *p++; t.rx = le16(p); p += 2; t.tx = le16(p); p += 2; t.hosted = le16(p); p += 2; t.uptime = le32(p);
    sTel[ses] = t;
    sTelDisc += t.disc; sTelFo += t.fo; sTelRj += t.rj; sTelHsf += t.hsf; sTelMsgs++;
}

static int pct(std::vector<int>& v, int p) {
    if (v.empty()) { return 0; }
    std::sort(v.begin(), v.end());
    size_t i = (size_t)((v.size() - 1) * p / 100);
    return v[i];
}

static void telem_flush(time_t now) {
    for (auto it = sTel.begin(); it != sTel.end();) {
        if (now - it->second.at > 90) { it = sTel.erase(it); } else { ++it; }
    }
    int act = 0, conn = 0, hosts = 0, coords = 0, sns = 0, natc[4] = { 0, 0, 0, 0 }, maxPlayers = 0;
    std::vector<int> fps, fpsMin, rx, tx, retx, hosted;
    for (auto& kv : sTel) {
        const Telem& t = kv.second;
        act++;
        if (t.flags & 8) { conn++; }
        if (t.flags & 1) { hosts++; }
        if (t.flags & 2) { coords++; }
        if (t.flags & 4) { sns++; hosted.push_back(t.hosted); }
        if (t.nat < 4) { natc[t.nat]++; }
        if (t.players > maxPlayers) { maxPlayers = t.players; }
        if (t.fps) { fps.push_back(t.fps); fpsMin.push_back(t.fpsMin); }
        rx.push_back(t.rx); tx.push_back(t.tx); retx.push_back(t.retx);
    }
    int fpsMed = pct(fps, 50), fpsP10 = pct(fps, 10), fpsMinP10 = pct(fpsMin, 10);
    int rxMed = pct(rx, 50), txMed = pct(tx, 50), txP90 = pct(tx, 90), retxMed = pct(retx, 50), retxP90 = pct(retx, 90);
    int hostedMax = hosted.empty() ? 0 : pct(hosted, 100);
    RV_LOG("telemetría: %d activos (%d conectados, %d hosts, %d coord., %d SN) | máx. jugadores %d | NAT sin=%d cono=%d sim=%d ? =%d | "
              "FPS med %d p10 %d mín p10 %d | último minuto: desconexiones %llu, cambios de SN %llu, reentradas %llu, apretones fallidos %llu | "
              "kbps rx %d tx %d (tx p90 %d) | retx %d%% (p90 %d%%)",
              act, conn, hosts, coords, sns, maxPlayers, natc[0], natc[1], natc[2], natc[3], fpsMed, fpsP10, fpsMinP10,
              sTelDisc, sTelFo, sTelRj, sTelHsf, rxMed, txMed, txP90, retxMed, retxP90);
    const char* jp = getenv("COOPNET_TELEMETRIA");
    std::string json = (jp && jp[0]) ? jp : "telemetria.json";
    FILE* f = fopen((json + ".tmp").c_str(), "w");
    if (f) {
        fprintf(f, "{\"hora\":%lld,\"activos\":%d,\"conectados\":%d,\"hosts\":%d,\"coordinadores\":%d,\"sn\":%d,"
                   "\"sn_alojados_max\":%d,\"max_jugadores\":%d,\"nat\":{\"sin\":%d,\"cono\":%d,\"simetrico\":%d,\"desconocido\":%d},"
                   "\"fps\":{\"mediana\":%d,\"p10\":%d,\"min_p10\":%d},"
                   "\"ultimo_minuto\":{\"informes\":%llu,\"desconexiones\":%llu,\"cambios_sn\":%llu,\"reentradas\":%llu,\"apretones_fallidos\":%llu},"
                   "\"kbps\":{\"rx_mediana\":%d,\"tx_mediana\":%d,\"tx_p90\":%d},\"retx_pct\":{\"mediana\":%d,\"p90\":%d},"
                   "\"encuentro\":{\"entradas\":%llu,\"entradas_bien\":%llu,\"registros\":%llu,\"descartados\":%llu}}\n",
                (long long)now, act, conn, hosts, coords, sns, hostedMax, maxPlayers, natc[0], natc[1], natc[2], natc[3],
                fpsMed, fpsP10, fpsMinP10, sTelMsgs, sTelDisc, sTelFo, sTelRj, sTelHsf, rxMed, txMed, txP90, retxMed, retxP90,
                sJoins, sJoinsOk, sRegs, sDropped);
        fclose(f);
        rename((json + ".tmp").c_str(), json.c_str());
    }
    std::string csv = json.size() > 5 && json.substr(json.size() - 5) == ".json" ? json.substr(0, json.size() - 5) + ".csv" : json + ".csv";
    bool fresh = false;
    { FILE* t = fopen(csv.c_str(), "r"); if (t) { fclose(t); } else { fresh = true; } }
    FILE* c = fopen(csv.c_str(), "a");
    if (c) {
        if (fresh) { fprintf(c, "hora,activos,conectados,hosts,coord,sn,max_jugadores,nat_sin,nat_cono,nat_sim,nat_desc,fps_med,fps_p10,fps_min_p10,desconexiones,cambios_sn,reentradas,apretones_fallidos,rx_med,tx_med,tx_p90,retx_med,retx_p90,entradas,entradas_bien\n"); }
        fprintf(c, "%lld,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%llu,%llu,%llu,%llu,%d,%d,%d,%d,%d,%llu,%llu\n",
                (long long)now, act, conn, hosts, coords, sns, maxPlayers, natc[0], natc[1], natc[2], natc[3], fpsMed, fpsP10, fpsMinP10,
                sTelDisc, sTelFo, sTelRj, sTelHsf, rxMed, txMed, txP90, retxMed, retxP90, sJoins, sJoinsOk);
        fclose(c);
    }
    sTelDisc = sTelFo = sTelRj = sTelHsf = sTelMsgs = 0;
}

static int open_udp(uint16_t port) {
    int s = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { return -1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr*)&a, sizeof a) != 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return -1;
    }
    return s;
}

static void put_hdr(uint8_t* b, uint8_t type) { memcpy(b, RV_MAGIC, 4); b[4] = type; }

static void send_to(int s, const struct sockaddr_in* to, const uint8_t* b, int n) {
    sendto(s, (const char*)b, n, 0, (const struct sockaddr*)to, sizeof *to);
}

static bool is_local(uint32_t ipn) {
    uint32_t h = ntohl(ipn);
    return (h >> 24) == 127 || (h >> 24) == 10 || (h >> 20) == 0xAC1 || (h >> 16) == 0xC0A8 || (h >> 22) == (100u << 2 | 1);   /* 127/8 10/8 172.16/12 192.168/16 100.64/10 */
}
static uint32_t local_to_public(uint32_t ipn) {
    static int init = 0; static uint32_t pub = 0;
    if (!init) {
        init = 1;
        const char* e = getenv("COOPNET_IP_PUBLICA");
        if (e && e[0]) { struct in_addr a; if (inet_pton(AF_INET, e, &a) == 1) pub = a.s_addr; }
    }
    return (pub && is_local(ipn)) ? pub : ipn;
}

static void handle(int which, const uint8_t* d, int n, const struct sockaddr_in* from) {
    if (n < 5 || memcmp(d, RV_MAGIC, 4) != 0) { return; }
    uint32_t ip = from->sin_addr.s_addr;
    uint16_t port = from->sin_port;
    if (!rate_ok(ip, port)) { sDropped++; return; }
    uint8_t type = d[4];
    uint8_t b[64];
    if (type == 1 && n >= 9) {                      /* SONDA */
        put_hdr(b, 2);
        memcpy(b + 5, d + 5, 4);
        memcpy(b + 9, &ip, 4);
        memcpy(b + 13, &port, 2);
        b[15] = (uint8_t)which;
        send_to(sSock[which], from, b, 16);
    } else if (type == 3 && n >= 31 && which == 0) {   /* REGISTRO */
        uint64_t lobbyId; memcpy(&lobbyId, d + 5, 8);
        uint16_t count; memcpy(&count, d + 29, 2);
        /* el host dedicado en la misma máquina que el servidor llega con una dirección local:
           a los que entran se les da la pública (COOPNET_IP_PUBLICA) */
        uint32_t regIp = local_to_public(ip);
        int rc = gServer->MassiveRegister(lobbyId, d + 13, count, regIp, port);
        sRegs++;
        put_hdr(b, 4);
        b[5] = (uint8_t)rc;
        memcpy(b + 6, &ip, 4);
        memcpy(b + 10, &port, 2);
        send_to(sSock[0], from, b, 12);
    } else if (type == 5 && n >= 18 && which == 0) {   /* ENTRAR */
        uint64_t lobbyId; memcpy(&lobbyId, d + 5, 8);
        int pl = d[17];
        if (18 + pl > n) { return; }
        std::string pw((const char*)d + 18, (size_t)pl);
        uint32_t hip = 0; uint16_t hport = 0;
        int rc = gServer->MassiveJoin(lobbyId, pw, &hip, &hport);
        sJoins++;
        put_hdr(b, 6);
        memcpy(b + 5, d + 13, 4);
        b[9] = (uint8_t)rc;
        memcpy(b + 10, &hip, 4);
        memcpy(b + 14, &hport, 2);
        send_to(sSock[0], from, b, 16);
        if (rc == 0) {
            sJoinsOk++;
            /* al host: que perfore hacia el que entra (dos veces, por si se pierde una) */
            struct sockaddr_in h;
            memset(&h, 0, sizeof h);
            h.sin_family = AF_INET;
            h.sin_addr.s_addr = hip;
            h.sin_port = hport;
            uint8_t p[11];
            put_hdr(p, 7);
            memcpy(p + 5, &ip, 4);
            memcpy(p + 9, &port, 2);
            send_to(sSock[0], &h, p, 11);
            send_to(sSock[0], &h, p, 11);
        }
    } else if (type == 8 && n >= 6) {               /* INFORME */
        if (d[5] < 4) { sNat[d[5]]++; }
    } else if (type == 9 && which == 0) {           /* TELEMETRÍA */
        telem_rx(d, n);
    } else if (type == 10 && n >= 9 && which == 0) {   /* VERSIÓN */
        const char* eb = getenv("COOPNET_FORK_BUILD");
        const char* ev = getenv("COOPNET_FORK_VERSION");
        const char* eu = getenv("COOPNET_FORK_URL");
        uint16_t build = (uint16_t)((eb && atoi(eb) > 0) ? atoi(eb) : 0);
        std::string lab = ev ? ev : "", url = eu ? eu : "";
        if (lab.size() > 60) { lab.resize(60); }
        if (url.size() > 200) { url.resize(200); }
        uint8_t r[300];
        put_hdr(r, 11);
        memcpy(r + 5, d + 5, 4);
        r[9] = (uint8_t)build; r[10] = (uint8_t)(build >> 8);
        int p = 11;
        r[p++] = (uint8_t)lab.size(); memcpy(r + p, lab.data(), lab.size()); p += (int)lab.size();
        r[p++] = (uint8_t)url.size(); memcpy(r + p, url.data(), url.size()); p += (int)url.size();
        send_to(sSock[0], from, r, p);
    }
}

static void loop() {
    time_t lastStats = time(NULL);
    uint8_t buf[1500];
    while (true) {
        fd_set fs;
        FD_ZERO(&fs);
        int mx = 0;
        for (int i = 0; i < 2; i++) { FD_SET(sSock[i], &fs); if (sSock[i] > mx) { mx = sSock[i]; } }
        struct timeval tv = { 1, 0 };
        int r = select(mx + 1, &fs, NULL, NULL, &tv);
        for (int i = 0; r > 0 && i < 2; i++) {
            if (!FD_ISSET(sSock[i], &fs)) { continue; }
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = (int)recvfrom(sSock[i], (char*)buf, sizeof buf, 0, (struct sockaddr*)&from, &fl);
            if (n > 0) { handle(i, buf, n, &from); }
        }
        time_t now = time(NULL);
        if (now - lastStats >= 60) {
            lastStats = now;
            RV_LOG("encuentro: NAT sin=%llu cono=%llu simétrico=%llu sin_respuesta=%llu | entradas %llu (bien %llu) | registros %llu | descartados %llu",
                sNat[0], sNat[1], sNat[2], sNat[3], sJoins, sJoinsOk, sRegs, sDropped);
            telem_flush(now);
        }
    }
}

bool rendezvous_begin(uint16_t aPortA, uint16_t aPortB) {
    sPort[0] = aPortA; sPort[1] = aPortB;
    sSock[0] = open_udp(aPortA);
    sSock[1] = open_udp(aPortB);
    if (sSock[0] < 0 || sSock[1] < 0) {
        RV_LOG("encuentro: no se pudieron abrir los puertos UDP %u y %u", aPortA, aPortB);
        return false;
    }
    gServer->mRendezvousPortA = aPortA;
    gServer->mRendezvousPortB = aPortB;
    std::thread(loop).detach();
    RV_LOG("encuentro: UDP en los puertos %u y %u", aPortA, aPortB);
    return true;
}
