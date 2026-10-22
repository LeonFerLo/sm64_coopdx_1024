/* Join a una partida masiva, primer paso: los mods de la partida.
   Antes de arrancar el mundo propio se entra a la red de smnet con un nodo aparte, se le pide el
   manifiesto al coordinador (el host), se baja lo que falta (de cualquiera que lo tenga, por las
   ramas de supernodos; modsync.c) a la carpeta de mods del usuario, se verifica y se activan
   exactamente esos mods. Recién ahí se sigue como siempre (ns_smnet_request_massive_join + Host).
   Si el host no contesta el manifiesto (versión vieja), se sigue con los mods propios. */
#ifdef SMNET
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ns_smnet.h"
#include "ns_smnet_lobby.h"
#include "modsync.h"
#include "smnet.h"
#include "smnet_udp.h"
#include "pc/djui/djui.h"
#include "pc/djui/djui_panel.h"
#include "pc/djui/djui_panel_menu.h"
#include "pc/djui/djui_panel_main.h"
#include "pc/mods/mods.h"
#include "pc/mods/mod.h"
#include "pc/fs/fs.h"
#include "pc/platform.h"
#include "pc/configfile.h"
#include "pc/debuglog.h"

#define PJ_MAX_BYTES (512u * 1048576u)   /* no se baja una partida de más de 512 MB de mods */

static struct {
    bool active;
    int fd;
    smnet_node_t* node;
    ms_dl_t* dl;
    bool started, checked;
    double t0;
    char ip[64];
    unsigned port;
    struct DjuiText* text;
    struct DjuiThreePanel* panel;
    char status[256];
    bool halted;
    /* Join desde la lista de coopnet: primero se le pregunta al encuentro dónde está el host */
    bool resolving;
    uint64_t lobbyId;
    char password[64];
    double rvSent, probeAt;
    int rvTries;
    uint16_t pubA, pubB; bool gotA, gotB, natDone;
} sPJ;

static void pj_on_send(void* user, const smnet_addr_t* to, const uint8_t* d, int len) { (void)user; smnet_udp_send(sPJ.fd, to, d, len); }
static int pj_send(void* u, uint16_t to, const uint8_t* d, int len) { (void)u; return smnet_node_send_msg(sPJ.node, SMNET_TO_PLAYER, to, d, len) == 0 ? 0 : -1; }

static void pj_paths(const ms_file_t* f, char* user, char* res, size_t n) {
    snprintf(user, n, "%s/%s", fs_get_write_path(MOD_DIRECTORY), f->path);
    snprintf(res, n, "%s/%s/%s", sys_resource_path(), MOD_DIRECTORY, f->path);
}
static bool same_file(const char* p, const ms_file_t* f) {
    uint32_t sz; uint8_t h[16];
    return ms_hash_file(p, &sz, h) == 0 && sz == f->size && memcmp(h, f->hash, 16) == 0;
}
static int pj_have(void* u, const ms_file_t* f) {
    (void)u;
    char a[1024], b[1024];
    pj_paths(f, a, b, sizeof a);
    return same_file(a, f) || same_file(b, f);
}
static void mkdirs_for(const char* path) {
    char t[1024];
    snprintf(t, sizeof t, "%s", path);
    for (char* s = t + 1; *s; s++) if (*s == '/' || *s == '\\') { char c = *s; *s = 0; if (!fs_sys_dir_exists(t)) fs_sys_mkdir(t); *s = c; }
}
static int pj_write(void* u, const ms_file_t* f, const uint8_t* data, uint32_t len) {
    (void)u;
    char a[1024], b[1024];
    pj_paths(f, a, b, sizeof a);
    mkdirs_for(a);
    FILE* o = fopen(a, "wb");
    if (!o) { LOG_ERROR("smnet: no puedo escribir %s", a); return -1; }
    size_t w = fwrite(data, 1, len, o);
    fclose(o);
    return w == len ? 0 : -1;
}
static void pj_on_msg(void* user, uint16_t from, smnet_scope_t scope, const uint8_t* d, int len, double now) {
    (void)user; (void)scope;
    if (sPJ.dl) ms_dl_on_msg(sPJ.dl, from, d, len, now);
}
static void pj_on_player(void* user, uint16_t id, int joined, const uint8_t* info, int len) {
    (void)user; (void)info; (void)len;
    if (joined && sPJ.dl) ms_dl_add_source(sPJ.dl, id);   /* cualquiera puede repartir; los bots no contestan y se dejan */
}

static void pj_stop_net(void);
static void pj_cleanup(void) {
    if (sPJ.node) { smnet_node_leave(sPJ.node, smnet_now_ms()); smnet_node_free(sPJ.node); }
    if (sPJ.fd >= 0) smnet_udp_close(sPJ.fd);
    ms_dl_free(sPJ.dl);
    sPJ.node = NULL; sPJ.dl = NULL; sPJ.fd = -1; sPJ.active = false;
}

/* ---- los mods propios del usuario: unirse activa los de la partida; los que tenía se guardan
   (smnet-mods-propios.txt) y vuelven al salir de la partida (o al arrancar, si el juego se cerró
   adentro). Si ya hay unos guardados no se pisan: son los del usuario, no los de otra partida. */
#define OWN_MODS_FILE "smnet-mods-propios.txt"
void ns_smnet_mods_save_own(void) {
    const char* path = fs_get_write_path(OWN_MODS_FILE);
    FILE* f = fopen(path, "r");
    if (f) { fclose(f); return; }
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "mods-propios v1\n");
    for (unsigned int i = 0; i < gLocalMods.entryCount; i++) {
        struct Mod* mod = gLocalMods.entries[i];
        if (mod && mod->enabled) fprintf(f, "%s\n", mod->relativePath);
    }
    fclose(f);
}
void ns_smnet_mods_restore_own(void) {
    const char* path = fs_get_write_path(OWN_MODS_FILE);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[512];
    if (!fgets(line, sizeof line, f) || strncmp(line, "mods-propios v1", 15) != 0) { fclose(f); remove(path); return; }
    for (unsigned int i = 0; i < gLocalMods.entryCount; i++) if (gLocalMods.entries[i]) gLocalMods.entries[i]->enabled = false;
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (!L) continue;
        for (unsigned int i = 0; i < gLocalMods.entryCount; i++)
            if (gLocalMods.entries[i] && !strcmp(gLocalMods.entries[i]->relativePath, line)) { gLocalMods.entries[i]->enabled = true; n++; break; }
    }
    fclose(f);
    remove(path);
    configfile_save(configfile_name());
    LOG_INFO("smnet: vuelven los mods propios (%d activos)", n);
}

/* ---- la clave del host: la primera vez que se entra a una partida por una dirección se recuerda
   (smnet-partidas-conocidas.txt); si después la lista viene firmada por otra clave, no se entra */
#define KNOWN_FILE "smnet-partidas-conocidas.txt"
static bool pj_known_key(const char* where, uint8_t out[32]) {
    FILE* f = fopen(fs_get_write_path(KNOWN_FILE), "r");
    if (!f) return false;
    char a[128], hx[80]; bool got = false;
    while (fscanf(f, "%127s %79s", a, hx) == 2) {
        if (strcmp(a, where) != 0 || strlen(hx) != 64) continue;
        for (int i = 0; i < 32; i++) { unsigned v; sscanf(hx + 2 * i, "%2x", &v); out[i] = (uint8_t)v; }
        got = true;   /* la última línea gana */
    }
    fclose(f);
    return got;
}
static void pj_remember_key(const char* where, const uint8_t pub[32]) {
    FILE* f = fopen(fs_get_write_path(KNOWN_FILE), "a");
    if (!f) return;
    char hx[65]; ms_hex(pub, 32, hx);
    fprintf(f, "%s %s\n", where, hx);
    fclose(f);
}
/* la lista tiene que venir firmada; la clave, la conocida para esa dirección (o se aprende) */
static bool pj_check_host(const ms_manifest_t* m, char* err, size_t errn) {
    if (!m->has_sig) { snprintf(err, errn, "La lista de mods de la partida no viene firmada por el host"); return false; }
    char where[96]; snprintf(where, sizeof where, "%s:%u", sPJ.ip, sPJ.port);
    uint8_t k[32];
    if (pj_known_key(where, k)) {
        if (memcmp(k, m->pub, 32) != 0) {
            snprintf(err, errn, "El host de %s no es el de antes (otra clave).\nSi es correcto, borrá su línea en " KNOWN_FILE, where);
            return false;
        }
    } else pj_remember_key(where, m->pub);
    return true;
}

/* corta la red (el panel queda con el error hasta Cancelar) */
static void pj_stop_net(void) {
    if (sPJ.node) { smnet_node_leave(sPJ.node, smnet_now_ms()); smnet_node_free(sPJ.node); sPJ.node = NULL; }
    if (sPJ.fd >= 0) { smnet_udp_close(sPJ.fd); sPJ.fd = -1; }
    ms_dl_free(sPJ.dl); sPJ.dl = NULL;
    sPJ.halted = true;
}

/* activa exactamente los mods del manifiesto, en su orden */
static bool pj_adopt(const ms_manifest_t* m, char* err, size_t errn) {
    mods_refresh_local();
    ns_smnet_mods_save_own();
    for (unsigned int i = 0; i < gLocalMods.entryCount; i++) gLocalMods.entries[i]->enabled = false;
    for (int k = 0; k < m->nmods; k++) {
        bool found = false;
        for (unsigned int i = 0; i < gLocalMods.entryCount; i++)
            if (!strcmp(gLocalMods.entries[i]->relativePath, m->mods[k])) { gLocalMods.entries[i]->enabled = true; found = true; break; }
        if (!found) { snprintf(err, errn, "falta el mod %s después de bajarlo", m->mods[k]); return false; }
    }
    return true;
}

static void pj_go(void) {
    extern void djui_panel_do_host_with_system(bool reconnecting, bool playSound, int system);
    char ip[64]; unsigned port = sPJ.port;
    snprintf(ip, sizeof ip, "%s", sPJ.ip);
    uint64_t lobby = sPJ.lobbyId; char pw[64]; snprintf(pw, sizeof pw, "%s", sPJ.password);
    pj_cleanup();
    ns_smnet_request_massive_join(ip, port);
    ns_smnet_set_join_lobby(lobby, pw);   /* 0 = Join por IP */
    djui_panel_do_host_with_system(false, true, NS_SOCKET);
}

static void pj_rv_rx(const uint8_t* d, int len);
static void pj_nat_update(double now);
static void pj_resolve_update(double now);

/* cada cuadro, desde network_update (no desde el dibujo del panel: pj_go cierra los paneles) */
void ns_smnet_prejoin_tick(void) {
    if (!sPJ.active || sPJ.halted) return;
    double now = smnet_now_ms();
    uint8_t buf[2048];
    smnet_addr_t from; int len;
    while ((len = smnet_udp_recv(sPJ.fd, &from, buf, sizeof buf)) >= 0) {
        if (ns_rv_is_msg(buf, len)) { pj_rv_rx(buf, len); continue; }
        if (sPJ.node) smnet_node_receive(sPJ.node, &from, buf, len, now);
    }
    if (sPJ.lobbyId) pj_nat_update(now);
    if (sPJ.resolving) { pj_resolve_update(now); return; }
    smnet_node_info_t inf; smnet_node_info(sPJ.node, &inf);
    if (!sPJ.started && inf.status == SMNET_PEER_CONNECTED && inf.coord_id != SMNET_NO_ID) { ms_dl_start(sPJ.dl, inf.coord_id, now); sPJ.started = true; }
    if (sPJ.started) ms_dl_update(sPJ.dl, now);
    smnet_node_update(sPJ.node, now);

    ms_dl_status_t st; ms_dl_status(sPJ.dl, &st);
    const ms_manifest_t* m = ms_dl_manifest(sPJ.dl);
    if (!sPJ.started) {
        snprintf(sPJ.status, sizeof sPJ.status, "Conectando a %s:%u...", sPJ.ip, sPJ.port);
        if (now - sPJ.t0 > 15000) { snprintf(sPJ.status, sizeof sPJ.status, "\\#ff8080\\%s:%u no responde", sPJ.ip, sPJ.port); }
    } else if (st.state == MS_DL_MANIFEST) {
        snprintf(sPJ.status, sizeof sPJ.status, "Pidiendo la lista de mods de la partida...");
        if (now - sPJ.t0 > 12000) {   /* host sin descarga de mods: con los propios */
            LOG_INFO("smnet: la partida no mandó lista de mods; se sigue con los mods propios");
            pj_go();
            return;
        }
    } else if (st.state == MS_DL_FILES || st.state == MS_DL_DONE) {
        if (m && !sPJ.checked) {
            char err[200];
            if (!pj_check_host(m, err, sizeof err)) { snprintf(sPJ.status, sizeof sPJ.status, "\\#ff8080\\%s", err); pj_stop_net(); return; }
            sPJ.checked = true;
        }
    }
    if (st.state == MS_DL_FILES) {
        if (m && m->total_bytes > PJ_MAX_BYTES) { snprintf(sPJ.status, sizeof sPJ.status, "\\#ff8080\\La partida tiene %.0f MB de mods (más de 512)", m->total_bytes / 1048576.0); return; }
        if (st.queue_pos > 0)
            snprintf(sPJ.status, sizeof sPJ.status, "En espera para bajar los mods: puesto %d\n%d de %d archivos, %.1f de %.1f MB",
                     st.queue_pos, st.files_done, st.files_needed, st.bytes_got / 1048576.0, st.bytes_needed / 1048576.0);
        else
            snprintf(sPJ.status, sizeof sPJ.status, "Bajando los mods de la partida (%d jugadores repartiendo)\n%d de %d archivos, %.1f de %.1f MB",
                     st.sources_alive, st.files_done, st.files_needed, st.bytes_got / 1048576.0, st.bytes_needed / 1048576.0);
        extern float gDownloadProgress;
        gDownloadProgress = st.bytes_needed ? (float)((double)st.bytes_got / (double)st.bytes_needed) : 0.f;
    } else if (st.state == MS_DL_FAILED) {
        snprintf(sPJ.status, sizeof sPJ.status, "\\#ff8080\\No se pudieron bajar los mods:\n%s", st.error);
        return;
    }
    if (st.state == MS_DL_DONE && m) {
        char err[160];
        LOG_INFO("smnet: mods de la partida listos (%d de %d archivos bajados)", st.files_needed, st.files_total);
        if (!pj_adopt(m, err, sizeof err)) { snprintf(sPJ.status, sizeof sPJ.status, "\\#ff8080\\%s", err); return; }
        ns_smnet_set_game_host(m->pub, m->sig, m->id);
        pj_go();
        return;
    }
}

static void pj_render_pre(struct DjuiBase* base, bool* skip) {
    (void)skip;
    if (sPJ.active && sPJ.text && base == &sPJ.text->base) djui_text_set_text(sPJ.text, sPJ.status);
}

static void pj_cancel(struct DjuiBase* caller) {
    pj_cleanup();
    djui_panel_menu_back(caller);
}
static bool pj_back(struct DjuiBase* caller) { pj_cancel(caller); return true; }

/* el nodo temporal del prejoin, hacia el host */
static bool pj_start_node(void) {
    smnet_addr_t coord;
    if (smnet_addr_parse(sPJ.ip, (uint16_t)sPJ.port, &coord) != 0) return false;
    smnet_node_cfg_t cfg;
    smnet_node_cfg_default(&cfg);
    if (smnet_random(cfg.seed, 32) != 0) return false;
    memcpy(&cfg.nonce, cfg.seed + 24, 4);
    cfg.upload_kbps = 1000;          /* de paso: que no lo elijan de SN */
    cfg.nat = SMNET_NAT_CONE;
    cfg.app_rate_direct = 3000;
    sPJ.node = smnet_node_new(&cfg, pj_on_send, NULL);
    if (!sPJ.node) return false;
    smnet_node_set_app_callbacks(sPJ.node, pj_on_msg, pj_on_player, NULL);
    { uint8_t mark = NS_SMNET_PREJOIN_MARK; smnet_node_set_player_info(sPJ.node, &mark, 1); }   /* no es un jugador (todavía) */
    ms_io_t io = { NULL, pj_send, NULL, pj_have, pj_write };
    sPJ.dl = ms_dl_new(&io);
    sPJ.t0 = smnet_now_ms();
    smnet_node_start(sPJ.node, &coord, sPJ.t0);
    return true;
}

static void pj_panel(struct DjuiBase* caller) {
    if (!gDjuiPanelMainCreated) { djui_panel_main_create(caller); }
    struct DjuiThreePanel* panel = djui_panel_menu_create(DLANG(JOIN_MESSAGE, JOINING), true);
    struct DjuiBase* body = djui_three_panel_get_body(panel);
    struct DjuiText* t = djui_text_create(body, "...");
    djui_base_set_size_type(&t->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
    djui_base_set_size(&t->base, 1.0f, 32 * 4);
    djui_base_set_color(&t->base, 220, 220, 220, 255);
    djui_text_set_alignment(t, DJUI_HALIGN_CENTER, DJUI_VALIGN_CENTER);
    t->base.on_render_pre = pj_render_pre;
    sPJ.text = t;
    extern float gDownloadProgress;
    gDownloadProgress = 0;
    djui_progress_bar_create(body, &gDownloadProgress, 0.0f, 1.0f, false);
    djui_button_create(body, DLANG(MENU, CANCEL), DJUI_BUTTON_STYLE_BACK, pj_cancel);
    panel->on_back = pj_back;
    djui_panel_add(caller, panel, NULL);
    sPJ.panel = panel;
}

bool ns_smnet_prejoin_begin(struct DjuiBase* caller, const char* ip, unsigned port) {
    if (sPJ.active) pj_cleanup();
    memset(&sPJ, 0, sizeof sPJ);
    sPJ.fd = -1;
    snprintf(sPJ.ip, sizeof sPJ.ip, "%s", (ip && ip[0]) ? ip : "127.0.0.1");
    sPJ.port = port ? port : 7777;
    sPJ.fd = smnet_udp_open(0);
    if (sPJ.fd < 0) return false;
    if (!pj_start_node()) { pj_cleanup(); return false; }
    sPJ.active = true;
    ns_smnet_set_join_lobby(0, NULL);
    pj_panel(caller);
    return true;
}

/* ---- Join desde la lista de coopnet: el encuentro da la dirección del host y le avisa que
   perfore hacia este socket; de paso se mide el NAT (para que el coordinador no nos haga SN si es
   simétrico) */
static void pj_rv_rx(const uint8_t* d, int len) {
    if (d[4] == RV_PROBE_R && len >= 16) {
        uint16_t p = (uint16_t)((d[13] << 8) | d[14]);
        if (d[15] == 0) { sPJ.pubA = p; sPJ.gotA = true; } else { sPJ.pubB = p; sPJ.gotB = true; }
    } else if (d[4] == RV_JOIN_R && len >= 16 && sPJ.resolving) {
        int st = d[9];
        if (st != 0) {
            snprintf(sPJ.status, sizeof sPJ.status, "\\#ff8080\\%s", ns_rv_join_error(st));
            pj_stop_net();
            return;
        }
        snprintf(sPJ.ip, sizeof sPJ.ip, "%u.%u.%u.%u", d[10], d[11], d[12], d[13]);
        sPJ.port = (unsigned)((d[14] << 8) | d[15]);
        sPJ.resolving = false;
        LOG_INFO("smnet: el encuentro dice que el host está en %s:%u", sPJ.ip, sPJ.port);
        if (!pj_start_node()) { snprintf(sPJ.status, sizeof sPJ.status, "\\#ff8080\\No se pudo arrancar la red"); pj_stop_net(); }
    }
}
static void pj_nat_update(double now) {
    if (sPJ.natDone || sPJ.probeAt <= 0) return;
    if (!(sPJ.gotA && sPJ.gotB) && now - sPJ.probeAt < 1500) return;
    smnet_addr_t me; uint16_t lp = 0;
    if (smnet_udp_local_addr(sPJ.fd, "0.0.0.0", &me) == 0) lp = smnet_addr_port(&me);
    int nat = ns_rv_nat_from_probes(lp, sPJ.pubA, sPJ.pubB, sPJ.gotA, sPJ.gotB);
    ns_smnet_set_nat_hint(nat);
    sPJ.natDone = true;
    smnet_addr_t a; uint8_t b[8];
    if (ns_rv_server_addr(0, &a)) { int n = ns_rv_build_report(b, nat); smnet_udp_send(sPJ.fd, &a, b, n); }
    LOG_INFO("smnet: NAT propio: %s", ns_rv_nat_name(nat));
}
static void pj_resolve_update(double now) {
    snprintf(sPJ.status, sizeof sPJ.status, "Buscando la partida...");
    if (now - sPJ.rvSent < 1000) return;
    if (sPJ.rvTries >= 8) {
        snprintf(sPJ.status, sizeof sPJ.status, "\\#ff8080\\El servidor de CoopNet no responde (puertos UDP del encuentro)");
        pj_stop_net();
        return;
    }
    smnet_addr_t a; uint8_t b[96];
    if (!ns_rv_server_addr(0, &a)) { snprintf(sPJ.status, sizeof sPJ.status, "\\#ff8080\\No se encuentra el servidor de CoopNet"); pj_stop_net(); return; }
    int n = ns_rv_build_join(b, sPJ.lobbyId, 500u + (uint32_t)sPJ.rvTries, sPJ.password);
    smnet_udp_send(sPJ.fd, &a, b, n);
    if (sPJ.rvTries == 0) {
        sPJ.probeAt = now;
        for (int k = 0; k < 2; k++) { smnet_addr_t s; if (ns_rv_server_addr(k, &s)) { n = ns_rv_build_probe(b, 200u + (uint32_t)k); smnet_udp_send(sPJ.fd, &s, b, n); } }
    }
    sPJ.rvSent = now; sPJ.rvTries++;
}

bool ns_smnet_prejoin_begin_lobby(struct DjuiBase* caller, uint64_t lobbyId, const char* password) {
    if (sPJ.active) pj_cleanup();
    memset(&sPJ, 0, sizeof sPJ);
    sPJ.fd = smnet_udp_open(0);
    if (sPJ.fd < 0) return false;
    sPJ.lobbyId = lobbyId;
    snprintf(sPJ.password, sizeof sPJ.password, "%s", password ? password : "");
    sPJ.resolving = true;
    sPJ.rvSent = -1e9;
    sPJ.active = true;
    sPJ.t0 = smnet_now_ms();
    snprintf(sPJ.status, sizeof sPJ.status, "Buscando la partida...");
    pj_panel(caller);
    return true;
}
#endif
