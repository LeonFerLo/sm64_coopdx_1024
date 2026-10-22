/* Moderación de las partidas masivas: ver ns_smnet_mod.h. */
#ifdef SMNET
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include "ns_smnet_mod.h"
#include "smnet.h"
#include "smnet_udp.h"
#include "pc/fs/fs.h"
#include "pc/debuglog.h"
#include "pc/djui/djui_chat_message.h"

#define KICK_MS      (5 * 60 * 1000.0)    /* echado: no puede volver por 5 min */
#define FOREVER      1e300
#define MAX_HIDDEN   128
#define MAX_MUTED    64
#define MAX_MODS     16
#define MAX_PKS      128
#define PKS_PER_MSG  8
#define BANS_FILE    "smnet-baneados.txt"

static struct { uint16_t id; double until; } sHidden[MAX_HIDDEN];
static int sNHidden;
static uint16_t sMuted[MAX_MUTED]; static int sNMuted;
static uint16_t sMods[MAX_MODS]; static int sNMods;
static struct { uint8_t pk[32]; double until; bool perm; } sPks[MAX_PKS];
static int sNPks, sPkRot;
static bool sClosed, sIsHost, sDirty;
static int sKicked;
static double sLastBcast, sLastApply;
static int sLastRole = -1;
/* administradores del host dedicado: identidades (COOPDX_SMNET_ADMINS, 64 hex cada una) que el host
   reconoce y a las que les da todos sus permisos (la partida del VPS no tiene quien escriba) */
static uint8_t sAdminPk[16][32]; static int sNAdminPk;
static uint16_t sAdmins[16]; static int sNAdmins;
static double sLastAdminScan;
static bool sIdWritten;

static double now_ms(void) { return smnet_now_ms(); }
static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
    char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    djui_chat_message_create(b);
}

/* ------------------------------------------------------------------ listas */

static int hidden_idx(uint16_t id) { for (int i = 0; i < sNHidden; i++) if (sHidden[i].id == id) return i; return -1; }
static void hidden_add(uint16_t id, double until) {
    int i = hidden_idx(id);
    if (i < 0) { if (sNHidden == MAX_HIDDEN) { memmove(sHidden, sHidden + 1, sizeof sHidden[0] * (MAX_HIDDEN - 1)); sNHidden--; } i = sNHidden++; }
    sHidden[i].id = id; sHidden[i].until = until;
}
static bool list_has(const uint16_t* l, int n, uint16_t id) { for (int i = 0; i < n; i++) if (l[i] == id) return true; return false; }
static void list_add(uint16_t* l, int* n, int cap, uint16_t id) { if (!list_has(l, *n, id) && *n < cap) l[(*n)++] = id; }
static void list_del(uint16_t* l, int* n, uint16_t id) { for (int i = 0; i < *n; i++) if (l[i] == id) { l[i] = l[--(*n)]; i--; } }
static void pk_add(const uint8_t pk[32], double until, bool perm) {
    for (int i = 0; i < sNPks; i++) if (memcmp(sPks[i].pk, pk, 32) == 0) {
        if (until > sPks[i].until) sPks[i].until = until;
        sPks[i].perm |= perm;
        return;
    }
    int i = sNPks < MAX_PKS ? sNPks++ : 0;
    memcpy(sPks[i].pk, pk, 32); sPks[i].until = until; sPks[i].perm = perm;
}

bool ns_smnet_mod_hidden(uint16_t id) {
    int i = hidden_idx(id);
    return i >= 0 && sHidden[i].until > now_ms();
}
bool ns_smnet_mod_muted(uint16_t id) { return list_has(sMuted, sNMuted, id) || ns_smnet_mod_hidden(id); }
bool ns_smnet_mod_closed(void) { return sClosed; }
bool ns_smnet_mod_is_moderator(uint16_t id) { return list_has(sMods, sNMods, id); }
bool ns_smnet_mod_can_moderate(void) { return sIsHost || ns_smnet_mod_is_moderator(ns_smnet_self_smnet_id()); }
bool ns_smnet_mod_is_admin(uint16_t id) { return list_has(sAdmins, sNAdmins, id); }
bool ns_smnet_mod_is_boss(void) { return sIsHost || ns_smnet_mod_is_admin(ns_smnet_self_smnet_id()); }
int  ns_smnet_mod_kicked(void) { return sKicked; }

/* baneos para siempre: una identidad (64 hex) por línea */
static void bans_save(void) {
    FILE* f = fopen(fs_get_write_path(BANS_FILE), "w");
    if (!f) return;
    fprintf(f, "# identidades baneadas para siempre de las partidas masivas (una por línea)\n");
    for (int i = 0; i < sNPks; i++) if (sPks[i].perm) {
        for (int k = 0; k < 32; k++) fprintf(f, "%02x", sPks[i].pk[k]);
        fprintf(f, "\n");
    }
    fclose(f);
}
static void bans_load(void) {
    FILE* f = fopen(fs_get_write_path(BANS_FILE), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        uint8_t pk[32]; int k = 0;
        for (; k < 32; k++) { unsigned v; if (sscanf(line + 2 * k, "%2x", &v) != 1) break; pk[k] = (uint8_t)v; }
        if (k == 32) pk_add(pk, FOREVER, true);
    }
    fclose(f);
}

void ns_smnet_mod_reset(bool isHost) {
    sNHidden = sNMuted = sNMods = sNPks = sPkRot = 0;
    sClosed = false; sDirty = true; sKicked = 0;
    sLastBcast = sLastApply = 0; sLastRole = -1;
    sIsHost = isHost;
    sNAdmins = 0; sNAdminPk = 0; sLastAdminScan = 0; sIdWritten = false;
    if (isHost) {
        bans_load();
        const char* e = getenv("COOPDX_SMNET_ADMINS");
        while (e && *e && sNAdminPk < 16) {
            while (*e && !isxdigit((unsigned char)*e)) e++;
            uint8_t pk[32]; int k = 0;
            for (; k < 32 && isxdigit((unsigned char)e[0]) && isxdigit((unsigned char)e[1]); k++, e += 2) { unsigned v; sscanf(e, "%2x", &v); pk[k] = (uint8_t)v; }
            if (k == 32) memcpy(sAdminPk[sNAdminPk++], pk, 32);
            while (*e && isxdigit((unsigned char)*e)) e++;
        }
        if (sNAdminPk) LOG_INFO("smnet: moderación: %d administrador(es) por identidad", sNAdminPk);
    }
}

/* ------------------------------------------------------------------ smnet (coordinador) */

static bool i_am_coord(void) {
    struct smnet_node* n = ns_smnet_node_ptr();
    if (!n) return false;
    smnet_node_info_t inf; smnet_node_info(n, &inf);
    return inf.coord_role == SMNET_COORD_ACTIVE;
}
static uint16_t coord_id(void) {
    struct smnet_node* n = ns_smnet_node_ptr();
    if (!n) return SMNET_NO_ID;
    smnet_node_info_t inf; smnet_node_info(n, &inf);
    return inf.coord_id;
}
static void send_to(uint16_t to, const uint8_t* b, int len) {
    struct smnet_node* n = ns_smnet_node_ptr();
    if (n && to != SMNET_NO_ID) smnet_node_send_msg(n, SMNET_TO_PLAYER, to, b, len);
}

/* las listas a smnet, si este nodo es el coordinador */
static void apply_to_smnet(double now) {
    struct smnet_node* n = ns_smnet_node_ptr();
    if (!n || !i_am_coord()) return;
    for (int i = 0; i < sNPks; i++) if (sPks[i].until > now) smnet_node_coord_block(n, sPks[i].pk, sPks[i].until >= FOREVER ? -1 : sPks[i].until - now, now);
    smnet_node_coord_set_closed(n, sClosed);
    for (int i = 0; i < sNHidden; i++) if (sHidden[i].until > now) smnet_node_coord_expel(n, sHidden[i].id, 0, now);
}

/* el coordinador echa a id y le devuelve la identidad al host (para la lista de baneos) */
static void coord_expel(uint16_t id, double block_ms, double now, bool perm) {
    struct smnet_node* n = ns_smnet_node_ptr();
    uint8_t pk[32];
    bool havePk = n && smnet_node_coord_identity(n, id, pk) == 0;
    if (n) smnet_node_coord_expel(n, id, block_ms, now);
    if (!havePk) return;
    if (sIsHost) { pk_add(pk, block_ms < 0 ? FOREVER : now + block_ms, perm); if (perm) bans_save(); sDirty = true; return; }
    uint16_t host = ns_smnet_host_smnet_id();
    uint8_t b[64]; b[0] = APP_MODREQ; b[1] = 0x80; memcpy(b + 2, &id, 2); memcpy(b + 4, pk, 32);
    int32_t secs = block_ms < 0 ? -1 : (int32_t)(block_ms / 1000); memcpy(b + 36, &secs, 4);
    send_to(host, b, 40);
}

/* ------------------------------------------------------------------ acciones (host) */

static void broadcast(double now) {
    struct smnet_node* n = ns_smnet_node_ptr();
    if (!n || !sIsHost) return;
    uint8_t b[SMNET_MAX_APP]; int p = 0;
    b[p++] = APP_MODLIST; b[p++] = 1; b[p++] = sClosed ? 1 : 0;
    int cnt = 0, cpos = p++;
    for (int i = 0; i < sNHidden && cnt < MAX_HIDDEN; i++) {
        double left = sHidden[i].until - now;
        if (left <= 0) continue;
        uint16_t secs = sHidden[i].until >= FOREVER ? 0xFFFF : (uint16_t)(left / 1000 > 65000 ? 65000 : left / 1000 + 1);
        memcpy(b + p, &sHidden[i].id, 2); memcpy(b + p + 2, &secs, 2); p += 4; cnt++;
    }
    b[cpos] = (uint8_t)cnt;
    b[p++] = (uint8_t)sNMuted; for (int i = 0; i < sNMuted; i++) { memcpy(b + p, &sMuted[i], 2); p += 2; }
    b[p++] = (uint8_t)sNMods; for (int i = 0; i < sNMods; i++) { memcpy(b + p, &sMods[i], 2); p += 2; }
    /* las identidades, de a PKS_PER_MSG por mensaje (rotando) */
    int npk = 0, kpos = p++;
    for (int k = 0; k < sNPks && npk < PKS_PER_MSG; k++) {
        int i = (sPkRot + k) % sNPks;
        if (sPks[i].until <= now) continue;
        uint16_t secs = sPks[i].until >= FOREVER ? 0xFFFF : (uint16_t)((sPks[i].until - now) / 1000 > 65000 ? 65000 : (sPks[i].until - now) / 1000 + 1);
        memcpy(b + p, sPks[i].pk, 32); memcpy(b + p + 32, &secs, 2); p += 34; npk++;
    }
    if (sNPks) sPkRot = (sPkRot + PKS_PER_MSG) % sNPks;
    b[kpos] = (uint8_t)npk;
    b[p++] = (uint8_t)sNAdmins; for (int i = 0; i < sNAdmins; i++) { memcpy(b + p, &sAdmins[i], 2); p += 2; }
    smnet_node_send_msg(n, SMNET_TO_ALL, 0, b, p);
}

static void exec_action(uint16_t id, int op, double now) {
    char name[40]; ns_smnet_player_name(id, name, sizeof name);
    if (op == NS_MOD_KICK || op == NS_MOD_BAN || op == NS_MOD_PERMBAN) {
        double block = op == NS_MOD_KICK ? KICK_MS : -1;
        hidden_add(id, op == NS_MOD_KICK ? now + KICK_MS : FOREVER);
        uint8_t k[2] = { APP_KICKED, (uint8_t)(op == NS_MOD_KICK ? 1 : 2) };
        send_to(id, k, 2);
        if (i_am_coord()) coord_expel(id, block, now, op == NS_MOD_PERMBAN);
        else { uint8_t r[8]; r[0] = APP_MODREQ; r[1] = 0x81; memcpy(r + 2, &id, 2); int32_t s = op == NS_MOD_KICK ? (int32_t)(KICK_MS / 1000) : -1; memcpy(r + 4, &s, 4); send_to(coord_id(), r, 8); }
        list_del(sMuted, &sNMuted, id); list_del(sMods, &sNMods, id);
        say("\\#ffa0a0\\%s: %s", op == NS_MOD_KICK ? "echado (5 min)" : op == NS_MOD_BAN ? "baneado de la partida" : "baneado para siempre", name);
        LOG_INFO("smnet: moderación: %s smnet:%u (%s)", op == NS_MOD_KICK ? "echado" : "baneado", id, name);
    } else if (op == NS_MOD_MUTE) { list_add(sMuted, &sNMuted, MAX_MUTED, id); say("\\#ffd080\\silenciado: %s", name); }
    else if (op == NS_MOD_UNMUTE) { list_del(sMuted, &sNMuted, id); say("\\#a0ffa0\\ya puede hablar: %s", name); }
    else if (op == NS_MOD_MODERATOR) { list_add(sMods, &sNMods, MAX_MODS, id); say("\\#a0d8ff\\moderador: %s", name); }
    else if (op == NS_MOD_UNMODERATOR) { list_del(sMods, &sNMods, id); say("\\#dcdcdc\\ya no es moderador: %s", name); }
    sDirty = true;
}

static uint16_t sPermPending[16]; static int sNPermPending;

bool ns_smnet_mod_action(uint16_t id, int op) {
    if (id >= SMNET_MAX_PLAYERS || id == ns_smnet_self_smnet_id()) return false;
    if (!ns_smnet_mod_can_moderate()) return false;
    uint16_t host = ns_smnet_host_smnet_id();
    bool boss = ns_smnet_mod_is_boss();
    if (!sIsHost && (id == host)) return false;                          /* al host no */
    if (ns_smnet_mod_is_admin(id) && !sIsHost) return false;             /* a un administrador, solo el host */
    if (!boss && (op == NS_MOD_PERMBAN || op == NS_MOD_MODERATOR || op == NS_MOD_UNMODERATOR)) return false;
    if (!boss && ns_smnet_mod_is_moderator(id) && op != NS_MOD_MUTE && op != NS_MOD_UNMUTE) return false;
    double now = now_ms();
    if (sIsHost) {
        if (op == NS_MOD_PERMBAN && !i_am_coord() && sNPermPending < 16) sPermPending[sNPermPending++] = id;
        exec_action(id, op, now);
        broadcast(now); sLastBcast = now; sDirty = false;
        return true;
    }
    uint8_t b[4] = { APP_MODREQ, (uint8_t)op, 0, 0 }; memcpy(b + 2, &id, 2);
    send_to(host, b, 4);
    return true;
}

static void boss_request(int op) {
    uint8_t b[4] = { APP_MODREQ, (uint8_t)op, 0, 0 };
    send_to(ns_smnet_host_smnet_id(), b, 4);
    say("\\#a0a0a0\\Pedido enviado al host.");
}

void ns_smnet_mod_set_closed(bool closed) {
    if (!sIsHost) { if (ns_smnet_mod_is_boss()) boss_request(closed ? NS_MOD_CLOSE : NS_MOD_OPEN); return; }
    sClosed = closed; sDirty = true;
    say(closed ? "\\#ffd080\\La partida está cerrada: solo pueden volver los que ya estaban." : "\\#a0ffa0\\La partida está abierta otra vez.");
    apply_to_smnet(now_ms());
}

void ns_smnet_mod_unban_all(void) {
    if (!sIsHost) { if (ns_smnet_mod_is_boss()) boss_request(NS_MOD_UNBAN_ALL); return; }
    sNHidden = 0; sNPks = 0; sDirty = true;
    struct smnet_node* n = ns_smnet_node_ptr();
    if (n) smnet_node_coord_unblock_all(n);
    bans_save();
    say("\\#a0ffa0\\Se sacaron todos los baneos.");
}

/* ------------------------------------------------------------------ mensajes */

void ns_smnet_mod_rx(uint16_t from, const uint8_t* d, int len, double now) {
    uint16_t host = ns_smnet_host_smnet_id();
    if (len < 2) return;
    if (d[0] == APP_MODLIST) {
        if (sIsHost || from != host || len < 4 || d[1] != 1) return;
        int p = 2;
        sClosed = d[p++] != 0;
        int nh = d[p++];
        if (p + nh * 4 > len) return;
        sNHidden = 0;
        for (int i = 0; i < nh; i++) {
            uint16_t id, secs; memcpy(&id, d + p, 2); memcpy(&secs, d + p + 2, 2); p += 4;
            hidden_add(id, secs == 0xFFFF ? FOREVER : now + secs * 1000.0);
        }
        if (p >= len) return;
        int nm = d[p++]; if (p + nm * 2 > len) return;
        sNMuted = 0; for (int i = 0; i < nm; i++) { uint16_t id; memcpy(&id, d + p, 2); p += 2; list_add(sMuted, &sNMuted, MAX_MUTED, id); }
        if (p >= len) return;
        int nd = d[p++]; if (p + nd * 2 > len) return;
        sNMods = 0; for (int i = 0; i < nd; i++) { uint16_t id; memcpy(&id, d + p, 2); p += 2; list_add(sMods, &sNMods, MAX_MODS, id); }
        if (p >= len) return;
        int nk = d[p++]; if (p + nk * 34 > len) return;
        for (int i = 0; i < nk; i++) {
            uint16_t secs; memcpy(&secs, d + p + 32, 2);
            pk_add(d + p, secs == 0xFFFF ? FOREVER : now + secs * 1000.0, false);
            p += 34;
        }
        if (p < len) {
            int na = d[p++]; sNAdmins = 0;
            for (int i = 0; i < na && p + 2 <= len && sNAdmins < 16; i++) { memcpy(&sAdmins[sNAdmins++], d + p, 2); p += 2; }
        }
        /* si a uno lo echaron y el aviso se perdió */
        uint16_t me = ns_smnet_self_smnet_id();
        if (me != SMNET_NO_ID && ns_smnet_mod_hidden(me) && !sKicked) {
            int i = hidden_idx(me);
            sKicked = (i >= 0 && sHidden[i].until >= FOREVER) ? 2 : 1;   /* para siempre: baneado */
        }
        apply_to_smnet(now);
    } else if (d[0] == APP_KICKED) {
        if (from != host || sIsHost) return;
        sKicked = d[1] == 2 ? 2 : 1;
    } else if (d[0] == APP_MODREQ) {
        int op = d[1];
        if (op == 0x81 && len >= 8 && from == host && i_am_coord()) {          /* host → coordinador: echar */
            uint16_t id; int32_t s; memcpy(&id, d + 2, 2); memcpy(&s, d + 4, 4);
            coord_expel(id, s < 0 ? -1 : s * 1000.0, now, false);
        } else if (op == 0x80 && len >= 40 && sIsHost && from == coord_id()) { /* coordinador → host: su identidad */
            uint16_t id; int32_t s; memcpy(&id, d + 2, 2); memcpy(&s, d + 36, 4);
            bool perm = false;
            for (int i = 0; i < sNPermPending; i++) if (sPermPending[i] == id) { perm = true; sPermPending[i] = sPermPending[--sNPermPending]; break; }
            pk_add(d + 4, s < 0 ? FOREVER : now + s * 1000.0, perm);
            if (perm) bans_save();
            sDirty = true;
        } else if (op >= NS_MOD_CLOSE && op <= NS_MOD_UNBAN_ALL && sIsHost && ns_smnet_mod_is_admin(from)) {   /* un administrador */
            if (op == NS_MOD_UNBAN_ALL) ns_smnet_mod_unban_all(); else ns_smnet_mod_set_closed(op == NS_MOD_CLOSE);
        } else if (op >= NS_MOD_KICK && op <= NS_MOD_UNMODERATOR && len >= 4 && sIsHost && ns_smnet_mod_is_admin(from)) {
            uint16_t id; memcpy(&id, d + 2, 2);
            if (id >= SMNET_MAX_PLAYERS || id == ns_smnet_self_smnet_id() || ns_smnet_mod_is_admin(id)) return;
            if (op == NS_MOD_PERMBAN && !i_am_coord() && sNPermPending < 16) sPermPending[sNPermPending++] = id;
            exec_action(id, op, now);
        } else if (op >= NS_MOD_KICK && op <= NS_MOD_UNMODERATOR && len >= 4 && sIsHost && ns_smnet_mod_is_moderator(from)) {
            uint16_t id; memcpy(&id, d + 2, 2);   /* un moderador pide */
            if (id >= SMNET_MAX_PLAYERS || id == ns_smnet_self_smnet_id() || ns_smnet_mod_is_moderator(id)) return;
            if (op == NS_MOD_PERMBAN || op == NS_MOD_MODERATOR || op == NS_MOD_UNMODERATOR) return;
            exec_action(id, op, now);
            char who[40]; ns_smnet_player_name(from, who, sizeof who);
            say("\\#a0a0a0\\(lo pidió el moderador %s)", who);
        }
    }
}

void ns_smnet_mod_update(double now) {
    struct smnet_node* n = ns_smnet_node_ptr();
    if (!n) return;
    /* la identidad propia, para poder darla de administrador en un host dedicado */
    if (!sIdWritten) {
        sIdWritten = true;
        uint8_t pk[32]; smnet_node_identity(n, pk);
        FILE* f = fopen(fs_get_write_path("smnet-identidad.txt"), "w");
        if (f) { for (int k = 0; k < 32; k++) fprintf(f, "%02x", pk[k]); fprintf(f, "\n"); fclose(f); }
    }
    /* el host: quiénes de los que están son administradores (cada 2 s, si es el coordinador) */
    if (sIsHost && sNAdminPk && now - sLastAdminScan > 2000 && i_am_coord()) {
        sLastAdminScan = now;
        uint16_t found[16]; int nf = 0;
        for (uint16_t id = 0; id < SMNET_MAX_PLAYERS && nf < 16; id++) {
            uint8_t pk[32];
            if (!ns_smnet_known_player(id) || smnet_node_coord_identity(n, id, pk) != 0) continue;
            for (int a = 0; a < sNAdminPk; a++) if (memcmp(pk, sAdminPk[a], 32) == 0) { found[nf++] = id; break; }
        }
        bool changed = nf != sNAdmins;
        for (int i = 0; i < nf && !changed; i++) if (!list_has(sAdmins, sNAdmins, found[i])) changed = true;
        if (changed) {
            for (int i = 0; i < nf; i++) if (!list_has(sAdmins, sNAdmins, found[i])) {
                char nm[40]; ns_smnet_player_name(found[i], nm, sizeof nm);
                LOG_INFO("smnet: moderación: entró el administrador smnet:%u (%s)", found[i], nm);
            }
            memcpy(sAdmins, found, sizeof found[0] * (size_t)nf); sNAdmins = nf;
            for (int i = 0; i < nf; i++) list_add(sMods, &sNMods, MAX_MODS, found[i]);
            sDirty = true;
        }
    }
    if (sIsHost && (sDirty || now - sLastBcast > 5000)) {
        broadcast(now); sLastBcast = now; sDirty = false;
    }
    /* si este nodo pasó a ser coordinador (o cada 5 s), las listas a smnet */
    smnet_node_info_t inf; smnet_node_info(n, &inf);
    if ((int)inf.coord_role != sLastRole || now - sLastApply > 5000) {
        sLastRole = (int)inf.coord_role; sLastApply = now;
        apply_to_smnet(now);
    }
}

/* ------------------------------------------------------------------ búsqueda y chat */

void ns_smnet_mod_name(uint16_t id, char* out, int cap) { ns_smnet_player_name(id, out, cap); }

static bool ieq(const char* a, const char* b) { for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false; return *a == *b; }
static bool iprefix(const char* s, const char* p) { for (; *p; s++, p++) if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*p)) return false; return true; }
static bool icontains(const char* s, const char* p) { if (!*p) return true; for (; *s; s++) if (iprefix(s, p)) return true; return false; }

int ns_smnet_mod_search(const char* text, uint16_t* out, int cap) {
    int n = 0;
    while (text && *text == ' ') text++;
    for (uint16_t id = 0; id < SMNET_MAX_PLAYERS && n < cap; id++) {
        if (!ns_smnet_known_player(id) || id == ns_smnet_self_smnet_id()) continue;
        char name[40]; ns_smnet_player_name(id, name, sizeof name);
        char num[8]; snprintf(num, sizeof num, "%u", id);
        if (!text || !*text || icontains(name, text) || !strcmp(num, text[0] == '#' ? text + 1 : text)) out[n++] = id;
    }
    return n;
}

/* "#12", "12" o el nombre (exacto o el único que empieza así) */
static int resolve(const char* who, uint16_t* out) {
    while (*who == ' ') who++;
    if (!*who) return 0;
    const char* num = who[0] == '#' ? who + 1 : who;
    char* end; long v = strtol(num, &end, 10);
    if (*num && *end == 0 && v >= 0 && v < SMNET_MAX_PLAYERS && ns_smnet_known_player((uint16_t)v)) { *out = (uint16_t)v; return 1; }
    int found = 0; uint16_t pre = SMNET_NO_ID; int npre = 0;
    for (uint16_t id = 0; id < SMNET_MAX_PLAYERS; id++) {
        if (!ns_smnet_known_player(id)) continue;
        char name[40]; ns_smnet_player_name(id, name, sizeof name);
        if (ieq(name, who)) { *out = id; found++; }
        else if (iprefix(name, who)) { pre = id; npre++; }
    }
    if (found == 1) return 1;
    if (found > 1) return -1;
    if (npre == 1) { *out = pre; return 1; }
    return npre > 1 ? -1 : 0;
}

static const struct { const char* cmd; int op; } sCmds[] = {
    { "/kick", NS_MOD_KICK }, { "/echar", NS_MOD_KICK },
    { "/ban", NS_MOD_BAN }, { "/banear", NS_MOD_BAN },
    { "/permban", NS_MOD_PERMBAN },
    { "/mute", NS_MOD_MUTE }, { "/silenciar", NS_MOD_MUTE },
    { "/unmute", NS_MOD_UNMUTE }, { "/desilenciar", NS_MOD_UNMUTE },
    { "/moderator", NS_MOD_MODERATOR }, { "/moderador", NS_MOD_MODERATOR },
    { "/unmoderator", NS_MOD_UNMODERATOR },
};

int ns_smnet_mod_command(const char* cmd) {
    if (!strcmp(cmd, "/help") || !strcmp(cmd, "/?") || !strcmp(cmd, "/ayuda")) {
        say("\\#fff982\\Partida masiva:");
        say("/jugadores [texto] - buscar jugadores (con su #número)");
        say("/miclave - tu identidad (para ser administrador de un host dedicado)");
        if (ns_smnet_mod_can_moderate()) {
            say("/kick NOMBRE|#N - echar (no puede volver por 5 min)");
            say("/ban NOMBRE|#N - banear de esta partida");
            say("/mute, /unmute NOMBRE|#N - silenciar el chat de alguien");
        }
        if (ns_smnet_mod_is_boss()) {
            say("/permban NOMBRE|#N - banear para siempre");
            say("/moderator, /unmoderator NOMBRE|#N - nombrar moderadores");
            say("/cerrar, /abrir - cerrar o abrir la entrada; /desbanear - sacar todos los baneos");
        }
        return 1;
    }
    if (!strncmp(cmd, "/jugadores", 10) || !strncmp(cmd, "/players", 8) || !strncmp(cmd, "/playerlist", 11)) {
        const char* q = strchr(cmd, ' ');
        uint16_t ids[12]; int n = ns_smnet_mod_search(q ? q + 1 : "", ids, 12);
        if (n == 0) { say("\\#ffa0a0\\No encontré a nadie."); return 1; }
        for (int i = 0; i < n; i++) {
            char name[40]; ns_smnet_player_name(ids[i], name, sizeof name);
            say("#%u %s%s%s", ids[i], name, ns_smnet_mod_muted(ids[i]) ? " \\#ffd080\\(silenciado)" : "", ns_smnet_mod_is_moderator(ids[i]) ? " \\#a0d8ff\\(moderador)" : "");
        }
        if (n == 12) say("\\#a0a0a0\\(hay más: escribí parte del nombre)");
        return 1;
    }
    if (!strcmp(cmd, "/miclave") || !strcmp(cmd, "/identidad")) {
        struct smnet_node* n = ns_smnet_node_ptr();
        if (!n) return 1;
        uint8_t pk[32]; smnet_node_identity(n, pk);
        char hx[65]; for (int k = 0; k < 32; k++) snprintf(hx + 2 * k, 3, "%02x", pk[k]);
        say("Tu identidad (para ADMINS del host dedicado; también está en smnet-identidad.txt):");
        say("%.32s", hx); say("%.32s", hx + 32);
        return 1;
    }
    if (!strcmp(cmd, "/cerrar") || !strcmp(cmd, "/abrir")) {
        if (!ns_smnet_mod_is_boss()) { say("\\#ffa0a0\\Solo el host puede cerrar o abrir la partida."); return 1; }
        ns_smnet_mod_set_closed(cmd[1] == 'c');
        return 1;
    }
    if (!strcmp(cmd, "/desbanear") || !strcmp(cmd, "/unban")) {
        if (!ns_smnet_mod_is_boss()) { say("\\#ffa0a0\\Solo el host puede sacar los baneos."); return 1; }
        ns_smnet_mod_unban_all();
        return 1;
    }
    for (size_t k = 0; k < sizeof sCmds / sizeof sCmds[0]; k++) {
        size_t l = strlen(sCmds[k].cmd);
        if (strncmp(cmd, sCmds[k].cmd, l) != 0 || (cmd[l] != ' ' && cmd[l] != 0)) continue;
        if (!ns_smnet_mod_can_moderate()) { say("\\#ffa0a0\\Solo el host o un moderador puede hacer eso."); return 1; }
        uint16_t id = SMNET_NO_ID;
        int r = resolve(cmd + l, &id);
        if (r == 0) { say("\\#ffa0a0\\No encontré a ese jugador (probá /jugadores)."); return 1; }
        if (r < 0) { say("\\#ffa0a0\\Hay varios con ese nombre: usá el #número (/jugadores)."); return 1; }
        if (!ns_smnet_mod_action(id, sCmds[k].op)) say("\\#ffa0a0\\No se puede (a uno mismo, al host o a un moderador, o hace falta ser el host).");
        else if (!sIsHost) say("\\#a0a0a0\\Pedido enviado al host.");
        return 1;
    }
    return 0;
}
#endif
