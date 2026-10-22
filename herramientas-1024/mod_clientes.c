/* smnet-mod-clientes: simulador de la descarga de mods entre jugadores (modsync).

   Semilla (hace de host, o se suma a una partida):
     smnet-mod-clientes --sembrar CARPETA_DE_MODS [--mods a,b,c] [--puerto 7777 | --unirse IP:PUERTO]
                        [--atender 4] [--tasa 400]
   Clientes falsos que bajan y verifican los mods:
     smnet-mod-clientes --clientes 20 --unirse IP:PUERTO [--guardar CARPETA] [--semillas]
                        [--atender 4] [--tasa 400] [--segundos 120] [--cada 150]

   Los clientes piden el manifiesto al coordinador de la partida y los pedazos a todas las semillas
   que encuentran (el coordinador, otras semillas y, con --semillas, los clientes que ya terminaron:
   así se reparte por las ramas). Cada segundo muestra cuántos terminaron, cuántos bajan, cuántos
   están en lista de espera y el tráfico de las semillas. Con --guardar, cada cliente escribe lo que
   baja en CARPETA/clienteN/ (si un archivo ya está bien, no lo baja). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
static void nap_ms(int ms) { Sleep((DWORD)ms); }
static int mkdir1(const char* p) { return _mkdir(p); }
#else
#include <time.h>
static void nap_ms(int ms) { struct timespec t = { 0, (long)ms * 1000000L }; nanosleep(&t, NULL); }
static int mkdir1(const char* p) { return mkdir(p, 0755); }
#endif
#include "smnet.h"
#include "smnet_udp.h"
#include "modsync.h"

#define MAXC 128

typedef struct {
    int idx;
    smnet_node_t* node;
    int fd;
    bool started, seeding, known_coord, reported;
    double start_at;
    ms_dl_t* dl;
    ms_seeder_t* sd;
    ms_manifest_t* man;          /* el de la semilla (propio o el bajado) */
    uint8_t** mem;               /* contenido de cada archivo, en memoria (semillas sin carpeta) */
    char dir[512];               /* donde están los archivos ("" = en memoria) */
} cli_t;

static cli_t C[MAXC + 1];
static int NC = 0;
static const char* gSaveDir = NULL;
static int gAtender = 4, gTasa = 400;
static bool gSemillas = false;
static bool gCorrupt = false;   /* --corromper: una semilla que manda datos alterados (para ver la verificación) */

static void on_send(void* user, const smnet_addr_t* to, const uint8_t* d, int len) { smnet_udp_send(((cli_t*)user)->fd, to, d, len); }

static int io_send(void* user, uint16_t to, const uint8_t* d, int len) {
    cli_t* c = user;
    return smnet_node_send_msg(c->node, SMNET_TO_PLAYER, to, d, len) == 0 ? 0 : -1;
}
static int io_read(void* user, int file, uint32_t off, uint8_t* buf, int len) {
    cli_t* c = user;
    if (c->mem && c->mem[file]) { memcpy(buf, c->mem[file] + off, (size_t)len); return len; }
    if (gCorrupt && c->idx == 0 && len > 0) {   /* datos al azar en vez del archivo */
        for (int k = 0; k < len; k++) buf[k] = (uint8_t)(rand() & 0xFF);
        return len;
    }
    const ms_manifest_t* m = c->man ? c->man : (c->dl ? ms_dl_manifest(c->dl) : NULL);
    if (!m) return -1;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", c->dir, m->files[file].path);
    FILE* f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, (long)off, SEEK_SET);
    int n = (int)fread(buf, 1, (size_t)len, f);
    fclose(f);
    return n;
}
static int io_have(void* user, const ms_file_t* f) {
    cli_t* c = user;
    if (!c->dir[0]) return 0;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", c->dir, f->path);
    uint32_t sz; uint8_t h[16];
    return ms_hash_file(p, &sz, h) == 0 && sz == f->size && memcmp(h, f->hash, 16) == 0;
}
static void mkdirs_for(const char* path) {
    char t[1024];
    snprintf(t, sizeof t, "%s", path);
    for (char* s = t + 1; *s; s++) if (*s == '/') { *s = 0; mkdir1(t); *s = '/'; }
}
static int io_write(void* user, const ms_file_t* f, const uint8_t* data, uint32_t len) {
    cli_t* c = user;
    if (!c->dir[0]) {   /* en memoria: se guarda para poder ser semilla después */
        const ms_manifest_t* m = ms_dl_manifest(c->dl);
        if (!c->mem) c->mem = calloc((size_t)m->nfiles, sizeof(uint8_t*));
        int i = (int)(f - m->files);
        c->mem[i] = malloc(len ? len : 1);
        memcpy(c->mem[i], data, len);
        return 0;
    }
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", c->dir, f->path);
    mkdirs_for(p);
    FILE* o = fopen(p, "wb");
    if (!o) return -1;
    size_t w = fwrite(data, 1, len, o);
    fclose(o);
    return w == len ? 0 : -1;
}

static void on_msg(void* user, uint16_t from, smnet_scope_t scope, const uint8_t* d, int len, double now) {
    (void)scope;
    cli_t* c = user;
    if (c->sd && ms_seeder_on_msg(c->sd, from, d, len, now)) return;
    if (c->dl) ms_dl_on_msg(c->dl, from, d, len, now);   /* también reparte lo que ya tiene (--semillas) */
}
static void on_player(void* user, uint16_t id, int joined, const uint8_t* info, int len) {
    (void)info; (void)len;
    cli_t* c = user;
    if (joined && c->dl) ms_dl_add_source(c->dl, id);   /* cualquiera puede ser semilla; los que no contestan se descartan */
}

static smnet_node_t* new_node(cli_t* c, uint16_t port) {
    c->fd = smnet_udp_open(port);
    if (c->fd < 0) { fprintf(stderr, "no pude abrir el puerto UDP %u\n", port); exit(1); }
    smnet_node_cfg_t cfg;
    smnet_node_cfg_default(&cfg);
    if (smnet_random(cfg.seed, 32) != 0) { fprintf(stderr, "sin entropía\n"); exit(1); }
    memcpy(&cfg.nonce, cfg.seed + 24, 4);
    cfg.upload_kbps = 30000;
    cfg.nat = SMNET_NAT_CONE;
    cfg.app_rate_direct = 3000;   /* la descarga va en mensajes directos */
    if (c->idx == 0 && port != 0) {   /* host de la partida (como ns_smnet.c) */
        cfg.is_host = 1;
        cfg.nat = SMNET_NAT_OPEN;
        char ip[16]; smnet_udp_lan_ip(ip);
        smnet_addr_parse(ip, port, &cfg.self_addr);
    }
    smnet_node_t* n = smnet_node_new(&cfg, on_send, c);
    if (!n) { fprintf(stderr, "no pude crear el nodo\n"); exit(1); }
    smnet_node_set_app_callbacks(n, on_msg, on_player, NULL);
    uint8_t info[32];
    int il = snprintf((char*)info, sizeof info, "%s%d", c->idx == 0 ? "Semilla" : "Cliente", c->idx);
    smnet_node_set_player_info(n, info, il);
    return n;
}

static void become_seeder(cli_t* c) {
    ms_io_t io = { c, io_send, io_read, io_have, io_write };
    c->sd = ms_seeder_new(c->man, &io, gAtender, gTasa);
    c->seeding = true;
}

int main(int argc, char** argv) {
    const char* seed_dir = NULL, * mods = NULL, * join = NULL;
    int port = 7777, nclients = 0;
    double seconds = 0, every = 150;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i], * v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--sembrar") && v) { seed_dir = v; i++; }
        else if (!strcmp(a, "--mods") && v) { mods = v; i++; }
        else if (!strcmp(a, "--puerto") && v) { port = atoi(v); i++; }
        else if (!strcmp(a, "--unirse") && v) { join = v; i++; }
        else if (!strcmp(a, "--clientes") && v) { nclients = atoi(v); i++; }
        else if (!strcmp(a, "--guardar") && v) { gSaveDir = v; i++; }
        else if (!strcmp(a, "--semillas")) { gSemillas = true; }
        else if (!strcmp(a, "--corromper")) { gCorrupt = true; }
        else if (!strcmp(a, "--atender") && v) { gAtender = atoi(v); i++; }
        else if (!strcmp(a, "--tasa") && v) { gTasa = atoi(v); i++; }
        else if (!strcmp(a, "--segundos") && v) { seconds = atof(v); i++; }
        else if (!strcmp(a, "--cada") && v) { every = atof(v); i++; }
        else { fprintf(stderr, "opción desconocida: %s\n", a); return 2; }
    }
    if ((!seed_dir && nclients <= 0) || (nclients > 0 && !join) || nclients > MAXC) {
        fprintf(stderr, "uso:\n  smnet-mod-clientes --sembrar CARPETA [--mods a,b] [--puerto P | --unirse IP:P] [--atender N] [--tasa PEDAZOS/s] [--corromper]\n"
                        "  smnet-mod-clientes --clientes N --unirse IP:P [--guardar CARPETA] [--semillas] [--atender N] [--tasa P/s] [--segundos S] [--cada MS]\n");
        return 2;
    }
    smnet_addr_t coord;
    if (join) {
        char ip[64]; unsigned jp = 7777;
        snprintf(ip, sizeof ip, "%s", join);
        char* c = strrchr(ip, ':'); if (c) { *c = 0; jp = (unsigned)atoi(c + 1); }
        if (smnet_addr_parse(ip, (uint16_t)jp, &coord) != 0) { fprintf(stderr, "no resuelvo %s\n", ip); return 1; }
    }
    double t0 = smnet_now_ms();

    /* semilla */
    static ms_manifest_t seedMan;
    if (seed_dir) {
        cli_t* c = &C[0];
        c->idx = 0;
        snprintf(c->dir, sizeof c->dir, "%s", seed_dir);
        ms_manifest_init(&seedMan);
        if (mods) {
            char buf[2048]; snprintf(buf, sizeof buf, "%s", mods);
            for (char* t = strtok(buf, ","); t; t = strtok(NULL, ",")) if (ms_manifest_add_mod(&seedMan, seed_dir, t) != 0) { fprintf(stderr, "no puedo leer el mod %s\n", t); return 1; }
        } else {
            /* todos los de la carpeta, en orden */
            const char* names[512]; int nn = 0;
            DIR* d = opendir(seed_dir);
            if (!d) { fprintf(stderr, "no puedo abrir %s\n", seed_dir); return 1; }
            struct dirent* e;
            while ((e = readdir(d)) && nn < 512) if (e->d_name[0] != '.') names[nn++] = strdup(e->d_name);
            closedir(d);
            for (int a = 0; a < nn; a++) for (int b = a + 1; b < nn; b++) if (strcmp(names[a], names[b]) > 0) { const char* t = names[a]; names[a] = names[b]; names[b] = t; }
            for (int a = 0; a < nn; a++) if (ms_manifest_add_mod(&seedMan, seed_dir, names[a]) != 0) fprintf(stderr, "aviso: no pude leer %s\n", names[a]);
        }
        ms_manifest_finish(&seedMan);
        char hx[33]; ms_hex(seedMan.id, 16, hx);
        printf("manifiesto %s: %d mods, %d archivos, %.1f MB\n", hx, seedMan.nmods, seedMan.nfiles, seedMan.total_bytes / 1048576.0);
        c->man = &seedMan;
        c->node = new_node(c, join ? 0 : (uint16_t)port);
        become_seeder(c);
        if (join) smnet_node_start(c->node, &coord, t0); else smnet_node_start(c->node, NULL, t0);
        c->started = true;
        printf("semilla %s\n", join ? "uniéndose a la partida" : "haciendo de host");
    }
    /* clientes falsos */
    for (int i = 1; i <= nclients; i++) {
        cli_t* c = &C[i];
        c->idx = i;
        if (gSaveDir) { snprintf(c->dir, sizeof c->dir, "%s/cliente%d", gSaveDir, i); mkdirs_for(c->dir); mkdir1(c->dir); }
        c->node = new_node(c, 0);
        ms_io_t io = { c, io_send, io_read, io_have, io_write };
        c->dl = ms_dl_new(&io);
        ms_dl_set_serving(c->dl, gSemillas, gAtender, gTasa);
        c->start_at = t0 + (seed_dir ? 1000 : 0) + i * every;
    }
    NC = nclients;
    uint8_t buf[2048];
    double last_print = t0;
    for (;;) {
        double now = smnet_now_ms();
        if (seconds > 0 && now - t0 > seconds * 1000) break;
        for (int i = seed_dir ? 0 : 1; i <= NC; i++) {
            cli_t* c = &C[i];
            if (!c->started) {
                if (now < c->start_at) continue;
                smnet_node_start(c->node, &coord, now);
                c->started = true;
            }
            smnet_addr_t from; int len;
            while ((len = smnet_udp_recv(c->fd, &from, buf, sizeof buf)) >= 0) smnet_node_receive(c->node, &from, buf, len, now);
            if (c->dl && !c->known_coord) {
                smnet_node_info_t inf; smnet_node_info(c->node, &inf);
                if (inf.status == SMNET_PEER_CONNECTED && inf.coord_id != SMNET_NO_ID) { ms_dl_start(c->dl, inf.coord_id, now); c->known_coord = true; }
            }
            if (c->dl && c->known_coord) {
                ms_dl_update(c->dl, now);
                ms_dl_status_t st; ms_dl_status(c->dl, &st);
                if ((st.state == MS_DL_DONE || st.state == MS_DL_FAILED) && !c->reported) {
                    c->reported = true;
                    if (st.state == MS_DL_DONE)
                        printf("  cliente %d: listo en %.1f s (%d de %d archivos bajados, %.1f MB, %d bloques con huella mala y repetidos, repartió %.1f MB mientras bajaba)\n", i,
                               (st.finished - st.started) / 1000.0, st.files_needed, st.files_total, st.bytes_needed / 1048576.0, st.blocks_bad, st.bytes_served / 1048576.0);
                    else printf("  cliente %d: FALLÓ: %s\n", i, st.error);
                    if (st.state == MS_DL_DONE) c->man = (ms_manifest_t*)ms_dl_manifest(c->dl);   /* para io_read desde la carpeta */
                }
            }
            if (c->sd) ms_seeder_update(c->sd, now);
            smnet_node_update(c->node, now);
        }
        if (now - last_print >= 1000) {
            last_print = now;
            int done = 0, failed = 0, dling = 0, waiting = 0, conn = 0, nsd = 0;
            uint64_t got = 0, need = 0;
            for (int i = 1; i <= NC; i++) {
                smnet_node_info_t inf; smnet_node_info(C[i].node, &inf);
                conn += inf.status == SMNET_PEER_CONNECTED;
                if (!C[i].dl) continue;
                ms_dl_status_t st; ms_dl_status(C[i].dl, &st);
                if (!C[i].known_coord) continue;
                if (st.state == MS_DL_DONE) done++;
                else if (st.state == MS_DL_FAILED) failed++;
                else if (st.queue_pos > 0) waiting++;
                else dling++;
                got += st.bytes_got; need += st.bytes_needed;
            }
            unsigned long long sbytes = 0, spz = 0; int satt = 0, swait = 0;
            for (int i = 0; i <= NC; i++) {
                if (C[i].sd) {
                    nsd++;
                    ms_seeder_stats_t ss; ms_seeder_stats(C[i].sd, &ss);
                    sbytes += ss.bytes; spz += ss.pedazos; satt += ss.atendiendo; swait += ss.en_espera;
                }
                if (C[i].dl) { ms_dl_status_t st; ms_dl_status(C[i].dl, &st); if (st.bytes_served > 0) { nsd++; sbytes += st.bytes_served; } }
            }
            if (NC > 0)
                printf("t=%4.0fs clientes conectados %d/%d: listos %d, bajando %d, en espera %d, fallaron %d | %.1f/%.1f MB | semillas %d: atendiendo %d, en espera %d, enviado %.1f MB\n",
                       (now - t0) / 1000.0, conn, NC, done, dling, waiting, failed, got / 1048576.0, need / 1048576.0, nsd, satt, swait, sbytes / 1048576.0);
            else
                printf("t=%4.0fs semilla: atendiendo %d, en espera %d, enviado %.1f MB (%llu pedazos)\n", (now - t0) / 1000.0, satt, swait, sbytes / 1048576.0, spz);
            if (NC > 0 && done + failed == NC && !gSemillas && !seed_dir) break;
        }
        nap_ms(2);
    }
    return 0;
}
