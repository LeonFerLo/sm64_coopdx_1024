/* smnet-mario-bots: bots que se ven en sm64coopdx (modo masivo).
 *
 * Cada bot es un nodo de smnet completo que se une a la partida y publica, como el juego, un
 * PLAYER de sm64coopdx como estado rico: una plantilla grabada desde el juego real
 * (COOPDX_SMNET_GRABAR=archivo) con la posición y el ángulo cambiados. Para el juego son
 * jugadores de verdad: ocupan slots, se dibujan como Marios completos y se les puede pegar.
 *
 * Uso:
 *   smnet-mario-bots --unirse IP[:puerto] --plantilla archivo [--bots N] [--radio U]
 *                    [--vuelta U] [--velocidad U/s] [--nombre Prefijo] [--segundos S] [--chat S]
 *                    [--entrada MS] [--saltos S]
 *
 * Hasta 1024 bots. --entrada MS: cada cuánto entra uno (150 por defecto; con 512, 25 o 50 para no
 * esperar más de un minuto).
 *
 * --chat S: cada bot manda un mensaje al chat global cada S segundos (en promedio).
 *
 * Rendimiento: por defecto los bots no verifican firmas, no abren atajos directos entre ellos y
 * esperan con poll (no preguntan a cada socket cada 2 ms); con todo eso gastan ~40 % menos de CPU
 * que antes. Para gastar todavía menos:
 *   --hz N: estado N veces por segundo (30 por defecto; con 15 o 20, casi la mitad de CPU, y los
 *           impostores se siguen moviendo suave porque se adelantan con la velocidad)
 *   --sn K: solo los primeros K bots pueden ser SN (con 256, 16 alcanzan)
 *   y varios procesos (un grupo por escenario) usan varios núcleos.
 *
 * --verificar: los bots verifican las firmas de los estados de sus vecinos como un jugador real
 * (por defecto no: es lo que más CPU gasta y para probar no hace falta).
 *
 * --saltos S: cada bot salta cada S segundos (en promedio), alternando salto, doble, triple y
 * mortal hacia atrás; sirve para ver los saltos de los impostores (solo los impostores: los
 * Marios completos de los bots repiten la plantilla).
 *
 * Los bots aparecen alrededor del lugar donde se grabó la plantilla (mismo nivel y área), cada
 * uno en un punto al azar dentro de --radio, y caminan en círculos de --vuelta unidades.
 * Conviene grabar la plantilla parado en un lugar plano y abierto.
 *
 * Compilar (Linux o MSYS2, desde juego/sm64coopdx): sh ../herramientas/compilar-bots.sh */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
/* esperar a que llegue algo a cualquiera de los sockets (en vez de preguntarle a cada uno cada 2 ms) */
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
static void nap_ms(int ms) { Sleep((DWORD)ms); }
typedef WSAPOLLFD pfd_t;
static int poll_all(pfd_t* p, int n, int ms) { return WSAPoll(p, (ULONG)n, ms); }
#define PFD_SET(p, s) do { (p).fd = (SOCKET)(s); (p).events = POLLRDNORM; (p).revents = 0; } while (0)
#define PFD_READY(p) ((p).revents & (POLLRDNORM | POLLERR | POLLHUP))
#else
#include <time.h>
#include <poll.h>
static void nap_ms(int ms) { struct timespec t = { 0, (long)ms * 1000000L }; nanosleep(&t, NULL); }
typedef struct pollfd pfd_t;
static int poll_all(pfd_t* p, int n, int ms) { return poll(p, (nfds_t)n, ms); }
#define PFD_SET(p, s) do { (p).fd = (s); (p).events = POLLIN; (p).revents = 0; } while (0)
#define PFD_READY(p) ((p).revents & (POLLIN | POLLERR | POLLHUP))
#endif
#include "smnet.h"
#include "smnet_udp.h"

#define MAXB 1024
#define PALETTE_SIZE 24      /* struct PlayerPalette: 8 partes × RGB */
#define FULL_K 24            /* igual que ns_smnet.c (SLOT_FULL_K) */

typedef struct {
    uint8_t pkt[3000];
    int len, data_off, off_pos, off_face, off_yaw, data_size;
    float x, y, z;
    uint16_t area_key;
} tmpl_t;

typedef struct {
    smnet_node_t* node;
    int fd;
    float cx, cz, r, phase, speed;
    double start_at, last_pub, next_chat, next_jump, jump_at, last_upd;
    int started, nchat, njump;
} bot_t;

static tmpl_t T;
static bot_t B[MAXB];
static int NB = 20;

static void on_send(void* user, const smnet_addr_t* to, const uint8_t* d, int len) {
    bot_t* b = (bot_t*)user;
    smnet_udp_send(b->fd, to, d, len);
}

static uint16_t area_key(unsigned level, unsigned area, unsigned act) {
    return (uint16_t)(((level & 0x3FF) << 6) | ((area & 7) << 3) | (act & 7));
}

static int load_template(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "no puedo abrir la plantilla %s\n", path); return -1; }
    char magic[4];
    uint16_t h[7];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "SMPL", 4) != 0 || fread(h, sizeof h, 1, f) != 1 || h[0] != 1) {
        fprintf(stderr, "%s no es una plantilla de PLAYER (grabala con COOPDX_SMNET_GRABAR)\n", path);
        fclose(f);
        return -1;
    }
    T.data_off = h[1]; T.off_pos = h[2]; T.off_face = h[3]; T.off_yaw = h[4]; T.data_size = h[5]; T.len = h[6];
    if (T.len <= 0 || T.len > (int)sizeof T.pkt - 4 || T.data_off + T.data_size > T.len ||
        fread(T.pkt, 1, (size_t)T.len, f) != (size_t)T.len) {
        fprintf(stderr, "plantilla dañada\n");
        fclose(f);
        return -1;
    }
    fclose(f);
    if (!(T.pkt[3] & 1) || T.pkt[0] != 1) { fprintf(stderr, "la plantilla no es un PLAYER con nivel y área\n"); return -1; }
    int loc = 5 + ((T.pkt[3] & 4) ? 5 : 0);
    int16_t level;
    memcpy(&level, &T.pkt[loc + 2], 2);
    T.area_key = area_key((unsigned)level, T.pkt[loc + 4], T.pkt[loc + 1]);
    memcpy(&T.x, &T.pkt[T.data_off + T.off_pos], 4);
    memcpy(&T.y, &T.pkt[T.data_off + T.off_pos + 4], 4);
    memcpy(&T.z, &T.pkt[T.data_off + T.off_pos + 8], 4);
    printf("plantilla: %d B, nivel %d área %u acto %u, en (%.0f, %.0f, %.0f)\n",
           T.len, level, T.pkt[loc + 4], T.pkt[loc + 1], T.x, T.y, T.z);
    return 0;
}

/* --area: niveles por nombre (número de nivel y de curso de sm64coopdx) */
static const struct { const char* name; int level, course; } kLevels[] = {
    { "jardines", 16, 0 }, { "castillo", 6, 0 }, { "patio", 26, 0 },
    { "bob", 9, 1 }, { "wf", 24, 2 }, { "jrb", 12, 3 }, { "ccm", 5, 4 }, { "bbh", 4, 5 },
    { "hmc", 7, 6 }, { "lll", 22, 7 }, { "ssl", 8, 8 }, { "ddd", 23, 9 }, { "sl", 10, 10 },
    { "wdw", 11, 11 }, { "ttm", 36, 12 }, { "thi", 13, 13 }, { "ttc", 14, 14 }, { "rr", 15, 15 },
    { "bitdw", 17, 16 }, { "bitfs", 19, 17 }, { "bits", 21, 18 }, { "pss", 27, 19 }, { "cotmc", 28, 20 },
    { "totwc", 29, 21 }, { "vcutm", 18, 22 }, { "wmotr", 31, 23 }, { "sa", 20, 24 },
};

/* --area NIVEL[:ÁREA[:ACTO]]: NIVEL es un nombre de la tabla o "número/curso" */
static int set_area(const char* spec) {
    char buf[64]; snprintf(buf, sizeof buf, "%s", spec);
    char* parts[3] = { buf, NULL, NULL };
    for (int k = 1; k < 3; k++) { char* c = parts[k - 1] ? strchr(parts[k - 1], ':') : NULL; if (c) { *c = 0; parts[k] = c + 1; } }
    int level = -1, course = 0;
    for (size_t i = 0; i < sizeof kLevels / sizeof kLevels[0]; i++) if (!strcmp(parts[0], kLevels[i].name)) { level = kLevels[i].level; course = kLevels[i].course; }
    if (level < 0) {
        char* sl = strchr(parts[0], '/');
        if (sl) { *sl = 0; course = atoi(sl + 1); }
        level = atoi(parts[0]);
    }
    if (level <= 0) { fprintf(stderr, "nivel desconocido: %s\n", parts[0]); return -1; }
    int area = parts[1] ? atoi(parts[1]) : 1;
    int act = parts[2] ? atoi(parts[2]) : (course > 0 && course <= 15 ? 1 : 0);
    int loc = 5 + ((T.pkt[3] & 4) ? 5 : 0);
    int16_t lv = (int16_t)level;
    T.pkt[loc] = (uint8_t)course; T.pkt[loc + 1] = (uint8_t)act;
    memcpy(&T.pkt[loc + 2], &lv, 2); T.pkt[loc + 4] = (uint8_t)area;
    T.area_key = area_key((unsigned)level, (unsigned)area, (unsigned)act);
    printf("bots en el nivel %d (curso %d), área %d, acto %d\n", level, course, area, act);
    return 0;
}

static double frand(uint32_t* s) { *s = *s * 1664525u + 1013904223u; return (double)(*s >> 8) / 16777216.0; }

int main(int argc, char** argv) {
    const char* join = NULL;
    const char* tpath = NULL;
    const char* prefix = "Bot";
    float radius = 1500, loop = 400, speed = 600;
    double seconds = 0, chat_every = 0, entry_ms = 150, jump_every = 0;
    int verify = 0, direct = 0, max_sn = -1;
    double pub_ms = 33;
    const char* areaSpec = NULL;
    const char* center = NULL;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--unirse") && v) { join = v; i++; }
        else if (!strcmp(a, "--plantilla") && v) { tpath = v; i++; }
        else if (!strcmp(a, "--bots") && v) { NB = atoi(v); i++; }
        else if (!strcmp(a, "--radio") && v) { radius = (float)atof(v); i++; }
        else if (!strcmp(a, "--vuelta") && v) { loop = (float)atof(v); i++; }
        else if (!strcmp(a, "--velocidad") && v) { speed = (float)atof(v); i++; }
        else if (!strcmp(a, "--nombre") && v) { prefix = v; i++; }
        else if (!strcmp(a, "--segundos") && v) { seconds = atof(v); i++; }
        else if (!strcmp(a, "--chat") && v) { chat_every = atof(v); i++; }
        else if (!strcmp(a, "--area") && v) { areaSpec = v; i++; }
        else if (!strcmp(a, "--centro") && v) { center = v; i++; }
        else if (!strcmp(a, "--saltos") && v) { jump_every = atof(v); i++; }
        else if (!strcmp(a, "--verificar")) { verify = 1; }
        else if (!strcmp(a, "--directos")) { direct = 1; }
        else if (!strcmp(a, "--sn") && v) { max_sn = atoi(v); i++; }
        else if (!strcmp(a, "--hz") && v) { double hz = atof(v); pub_ms = hz >= 5 && hz <= 60 ? 1000.0 / hz : 33; i++; }
        else if (!strcmp(a, "--entrada") && v) { entry_ms = atof(v); if (entry_ms < 0) entry_ms = 0; i++; }
        else { fprintf(stderr, "opción desconocida: %s\n", a); return 2; }
    }
    if (!join || !tpath || NB < 1 || NB > MAXB) {
        fprintf(stderr, "uso: smnet-mario-bots --unirse IP[:puerto] --plantilla archivo [--bots 1..%d] [--radio U] [--vuelta U] [--velocidad U/s] [--nombre P] [--segundos S] [--chat S] [--entrada MS] [--saltos S]\n"
                        "       [--area NIVEL[:AREA[:ACTO]]] (jardines, castillo, patio, bob, wf, jrb, ccm, bbh, hmc, lll, ssl, ddd, sl, wdw, ttm, thi, ttc, rr, ... o NUMERO/CURSO)\n"
                        "       [--centro X,Y,Z] (dónde se reparten; por defecto, donde se grabó la plantilla)\n", MAXB);
        return 2;
    }
    if (load_template(tpath) != 0) return 1;
    if (areaSpec && set_area(areaSpec) != 0) return 2;
    if (center) {
        float cx, cy, cz;
        if (sscanf(center, "%f,%f,%f", &cx, &cy, &cz) != 3) { fprintf(stderr, "--centro X,Y,Z\n"); return 2; }
        T.x = cx; T.y = cy; T.z = cz;
    } else if (areaSpec) {
        printf("aviso: sin --centro los bots quedan en la posición de la plantilla (%.0f, %.0f, %.0f), que en otro nivel puede caer fuera del mapa\n", T.x, T.y, T.z);
    }
    char ip[64];
    unsigned port = 7777;
    snprintf(ip, sizeof ip, "%s", join);
    char* c = strrchr(ip, ':');
    if (c) { *c = '\0'; port = (unsigned)atoi(c + 1); }
    smnet_addr_t coord;
    if (smnet_addr_parse(ip, (uint16_t)port, &coord) != 0) { fprintf(stderr, "no resuelvo %s\n", ip); return 1; }

    uint32_t rs = 12345;
    { uint8_t r[4]; if (smnet_random(r, 4) == 0) memcpy(&rs, r, 4); }
    double t0 = smnet_now_ms();
    for (int i = 0; i < NB; i++) {
        bot_t* b = &B[i];
        b->fd = smnet_udp_open(0);
        if (b->fd < 0) { fprintf(stderr, "no pude abrir un puerto UDP para el bot %d\n", i); return 1; }
        smnet_node_cfg_t cfg;
        smnet_node_cfg_default(&cfg);
        if (smnet_random(cfg.seed, 32) != 0) { fprintf(stderr, "sin entropía\n"); return 1; }
        memcpy(&cfg.nonce, cfg.seed + 24, 4);
        cfg.upload_kbps = (max_sn < 0 || i < max_sn) ? 30000 : 10000;   /* --sn K: solo los primeros K pueden ser SN */
        cfg.verify_neighbors = verify;   /* los bots no verifican las firmas de sus vecinos (lo que más CPU gastaba) */
        cfg.direct = direct;             /* ni abren atajos directos entre ellos: todo por los SN (menos apretones de manos) */
        cfg.nat = SMNET_NAT_CONE;
        cfg.app_rate_direct = 3000;
        cfg.sn.full_k = FULL_K;
        cfg.sn.full_radius = 1e7f;
        cfg.sn.full_budget_kbps = 1200;
        cfg.sn.tier_k[0] = 64; cfg.sn.tier_k[1] = 176; cfg.sn.down_budget_kbps = 900;   /* igual que ns_smnet.c */
        b->node = smnet_node_new(&cfg, on_send, b);
        if (!b->node) { fprintf(stderr, "no pude crear el bot %d\n", i); return 1; }
        /* datos de presentación como los del juego: modelo, paleta, nombre */
        uint8_t info[SMNET_MAX_PINFO];
        int n = 0;
        info[n++] = 0;
        for (int k = 0; k < PALETTE_SIZE; k++) info[n++] = (uint8_t)(40 + frand(&rs) * 215);
        n += snprintf((char*)info + n, (size_t)(SMNET_MAX_PINFO - n), "%s%d", prefix, i + 1);
        smnet_node_set_player_info(b->node, info, n);
        float a = (float)(frand(&rs) * 6.2831853), d = (float)(sqrt(frand(&rs)) * radius);
        b->cx = T.x + d * cosf(a);
        b->cz = T.z + d * sinf(a);
        b->r = loop * (float)(0.5 + frand(&rs));
        b->phase = (float)(frand(&rs) * 6.2831853);
        b->speed = speed * (float)(0.7 + 0.6 * frand(&rs));
        b->start_at = t0 + i * entry_ms;   /* entran de a uno, como gente */
        b->next_chat = b->start_at + 5000 + frand(&rs) * chat_every * 1000;
        b->next_jump = b->start_at + 3000 + frand(&rs) * jump_every * 1000; b->jump_at = 0;
    }
    printf("%d bots uniéndose a %s:%u\n", NB, ip, port);

    uint8_t buf[2048], pkt[3004], zb[4096];
    double last_print = t0;
    pfd_t* P = calloc((size_t)NB, sizeof *P);
    if (!P) { fprintf(stderr, "sin memoria\n"); return 1; }
    for (int i = 0; i < NB; i++) PFD_SET(P[i], B[i].fd);
    for (;;) {
        /* espera hasta 5 ms a que llegue algo; después, solo se lee de los sockets que tienen datos
           y cada nodo se actualiza cada 10 ms (o al recibir) */
        for (int i = 0; i < NB; i++) P[i].revents = 0;
        int ready = poll_all(P, NB, 5);
        double now = smnet_now_ms();
        if (seconds > 0 && now - t0 > seconds * 1000) break;
        for (int i = 0; i < NB; i++) {
            bot_t* b = &B[i];
            if (!b->started) {
                if (now < b->start_at) continue;
                smnet_node_start(b->node, &coord, now);
                b->started = 1;
            }
            smnet_addr_t from;
            int len, got = 0;
            if (ready < 0 || PFD_READY(P[i]))
                while ((len = smnet_udp_recv(b->fd, &from, buf, sizeof buf)) >= 0) { smnet_node_receive(b->node, &from, buf, len, now); got = 1; }
            if (now - b->last_pub >= pub_ms) {
                b->last_pub = now;
                /* da vueltas: posición sobre el círculo y mirando hacia donde camina */
                float w = b->speed / b->r;
                float ang = b->phase + w * (float)((now - t0) / 1000.0);
                float x = b->cx + b->r * cosf(ang), z = b->cz + b->r * sinf(ang), y = T.y;
                /* tangente (-sin, cos): yaw de SM64 = atan2(dx, dz) en unidades de 65536 por vuelta */
                float yawf = atan2f(-sinf(ang), cosf(ang)) * (32768.0f / 3.14159265f);
                int16_t yaw = (int16_t)(int)yawf;
                memcpy(pkt, T.pkt, (size_t)T.len);
                memcpy(&pkt[T.data_off + T.off_pos], &x, 4);
                memcpy(&pkt[T.data_off + T.off_pos + 4], &y, 4);
                memcpy(&pkt[T.data_off + T.off_pos + 8], &z, 4);
                memcpy(&pkt[T.data_off + T.off_face + 2], &yaw, 2);   /* faceAngle[1] */
                memcpy(&pkt[T.data_off + T.off_yaw], &yaw, 2);        /* intendedYaw */
                memset(&pkt[T.len], 0, 4);                            /* el hash (no se verifica) */
                uLongf zl = sizeof zb;
                if (compress2(zb, &zl, pkt, (uLong)T.len + 4, 1) == Z_OK) {
                    smnet_state_t st;
                    memset(&st, 0, sizeof st);
                    st.x = x; st.y = y; st.z = z; st.yaw = yaw; st.fwd_vel = b->speed / 30.0f;
                    st.action = st.fwd_vel < 12.f ? 0x48 : 0x72; st.flags = 0x08;   /* caminando / corriendo */
                    /* salto: parábola de SM64 (impulso inicial, gravedad -4 por cuadro); la pose
                       del byte action como la manda ns_smnet.c (bit 7 = en el aire) */
                    if (jump_every > 0 && b->jump_at == 0 && now >= b->next_jump) { b->jump_at = now; b->njump++; }
                    if (b->jump_at > 0) {
                        static const float v0[4] = { 42.f, 52.f, 69.f, 62.f };
                        /* animación (CharacterAnimID): salto, doble, triple, mortal atrás */
                        static const uint8_t anim[4] = { 0x4D, 0x50, 0xC1, 0x04 };
                        int k = (b->njump - 1) & 3;
                        float fr = (float)((now - b->jump_at) / (1000.0 / 30.0));
                        float h = v0[k] * fr - 2.f * fr * fr;
                        if (h <= 0.f && fr > 1.f) {
                            b->jump_at = 0;
                            b->next_jump = now + jump_every * 1000 * (0.5 + frand(&rs));
                        } else {
                            st.y = y + (h > 0.f ? h : 0.f);
                            st.vel_y = v0[k] - 4.f * fr;
                            st.action = anim[k];
                            st.anim_frame = (uint8_t)(fr > 60.f ? 60 : fr);
                            st.flags = 0x08 | 0x01 | 0x02;   /* formato 2, en el aire, con su altura (ns_smnet.c, STF_*) */
                        }
                    }
                    smnet_node_set_local(b->node, T.area_key, &st, zb, (int)zl);
                }
            }
            if (chat_every > 0 && now >= b->next_chat) {
                smnet_node_info_t inf;
                smnet_node_info(b->node, &inf);
                if (inf.status == SMNET_PEER_CONNECTED) {
                    uint8_t m[96];
                    int ml = snprintf((char*)m + 1, sizeof m - 1, "hola, soy %s%d (mensaje %d)", prefix, i + 1, ++b->nchat);
                    m[0] = 0x81;   /* chat del modo masivo (ns_smnet.c, APP_CHAT) */
                    smnet_node_send_msg(b->node, SMNET_TO_ALL, 0, m, ml + 1);
                }
                b->next_chat = now + chat_every * 1000 * (0.5 + frand(&rs));
            }
            if (got || now - b->last_upd >= 10) { b->last_upd = now; smnet_node_update(b->node, now); }
        }
        if (now - last_print > 5000) {
            last_print = now;
            int conn = 0, sn = 0, coords = 0, otro = 0; unsigned rej = 0, fo = 0;
            for (int i = 0; i < NB; i++) {
                smnet_node_info_t inf;
                smnet_node_info(B[i].node, &inf);
                conn += inf.status == SMNET_PEER_CONNECTED;
                sn += inf.is_sn;
                rej += inf.rejoins; fo += inf.failovers;
                coords += inf.coord_role == SMNET_COORD_ACTIVE;
                otro += inf.status == SMNET_PEER_CONNECTED && inf.coord_id != 0;
            }
            printf("t=%3.0fs bots conectados %d/%d, SN entre los bots %d, reentradas %u, cambios de SN %u%s\n", (now - t0) / 1000.0, conn, NB, sn, rej, fo,
                   coords ? " (¡hay bots de coordinador: la partida se partió!)" : "");
            if (otro) printf("      %d bots con otro coordinador que el del Join\n", otro);
            fflush(stdout);
        }
    }
    free(P);
    for (int i = 0; i < NB; i++) { smnet_node_leave(B[i].node, smnet_now_ms()); }
    for (int k = 0; k < 20; k++) {
        double now = smnet_now_ms();
        for (int i = 0; i < NB; i++) smnet_node_update(B[i].node, now);
        nap_ms(5);
    }
    for (int i = 0; i < NB; i++) { smnet_node_free(B[i].node); smnet_udp_close(B[i].fd); }
    printf("listo\n");
    return 0;
}
