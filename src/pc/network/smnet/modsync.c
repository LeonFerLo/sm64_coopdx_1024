/* modsync: ver modsync.h */
#include "modsync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "../../../../lib/smnet/third_party/monocypher/monocypher.h"

#define MS_PART      1080     /* bytes de manifiesto por mensaje */
#define MS_WINDOW    24       /* pedazos pedidos y sin llegar, por semilla */
#define MS_PROBATION 2        /* … hasta que contesta por primera vez */
#define MS_TIMEOUT   4000.0
#define MS_PER_REQ   64       /* pedidos por mensaje */

/* ------------------------------------------------------------------ huellas */
void ms_hash_mem(const uint8_t* d, uint32_t len, uint8_t hash[16]) { crypto_blake2b(hash, 16, d, len); }

int ms_hash_file(const char* path, uint32_t* size, uint8_t hash[16]) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    crypto_blake2b_ctx c;
    crypto_blake2b_init(&c, 16);
    uint8_t buf[65536];
    uint64_t tot = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) { crypto_blake2b_update(&c, buf, n); tot += n; }
    fclose(f);
    if (tot > 0xFFFFFFFFu) return -1;
    crypto_blake2b_final(&c, hash);
    *size = (uint32_t)tot;
    return 0;
}

void ms_hex(const uint8_t* h, int n, char* out) {
    static const char* x = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2 * i] = x[h[i] >> 4]; out[2 * i + 1] = x[h[i] & 15]; }
    out[2 * n] = 0;
}

/* ------------------------------------------------------------------ firmas */
void ms_key_pair(uint8_t sk[64], uint8_t pub[32], const uint8_t seed[32]) {
    uint8_t s[32]; memcpy(s, seed, 32);          /* monocypher borra la semilla */
    crypto_eddsa_key_pair(sk, pub, s);
}
void ms_sign(uint8_t sig[64], const uint8_t sk[64], const uint8_t* msg, int len) { crypto_eddsa_sign(sig, sk, msg, (size_t)len); }
int ms_check(const uint8_t sig[64], const uint8_t pub[32], const uint8_t* msg, int len) { return crypto_eddsa_check(sig, pub, msg, (size_t)len) == 0 ? 0 : -1; }

#define MS_SIG_TAG "MSS1"
static void sig_msg(const uint8_t id[16], uint8_t out[20]) { memcpy(out, "MSIG", 4); memcpy(out + 4, id, 16); }
static int body_len(const ms_manifest_t* m) { return m->has_sig ? m->blob_len - 100 : m->blob_len; }
int ms_manifest_attach_sig(ms_manifest_t* m, const uint8_t pub[32], const uint8_t sig[64]) {
    uint8_t msg[20]; sig_msg(m->id, msg);
    if (ms_check(sig, pub, msg, 20) != 0) return -1;
    int bl = body_len(m);
    uint8_t* nb = realloc(m->blob, (size_t)bl + 100);
    if (!nb) return -1;
    m->blob = nb;
    memcpy(m->blob + bl, MS_SIG_TAG, 4); memcpy(m->blob + bl + 4, pub, 32); memcpy(m->blob + bl + 36, sig, 64);
    m->blob_len = bl + 100;
    memcpy(m->pub, pub, 32); memcpy(m->sig, sig, 64); m->has_sig = 1;
    return 0;
}
void ms_manifest_sign(ms_manifest_t* m, const uint8_t sk[64], const uint8_t pub[32]) {
    uint8_t msg[20], sig[64]; sig_msg(m->id, msg);
    ms_sign(sig, sk, msg, 20);
    ms_manifest_attach_sig(m, pub, sig);
}

/* ------------------------------------------------------------------ manifiesto */
void ms_manifest_init(ms_manifest_t* m) { memset(m, 0, sizeof *m); }
void ms_manifest_free(ms_manifest_t* m) { free(m->mods); free(m->files); free(m->bhash); free(m->blob); memset(m, 0, sizeof *m); }

static int add_file(ms_manifest_t* m, const char* full, const char* rel) {
    ms_file_t f;
    memset(&f, 0, sizeof f);
    if (strlen(rel) >= sizeof f.path) return -1;
    snprintf(f.path, sizeof f.path, "%s", rel);
    if (ms_hash_file(full, &f.size, f.hash) != 0) return -1;
    /* huella de cada bloque */
    f.block0 = m->nblocks;
    f.nblocks = (f.size + MS_BLOCK - 1) / MS_BLOCK;
    if (f.nblocks) {
        uint8_t (*nb)[16] = realloc(m->bhash, 16 * (size_t)(m->nblocks + f.nblocks));
        if (!nb) return -1;
        m->bhash = nb;
        FILE* fp = fopen(full, "rb");
        if (!fp) return -1;
        uint8_t* buf = malloc(MS_BLOCK);
        for (uint32_t b = 0; b < f.nblocks; b++) {
            size_t n = fread(buf, 1, MS_BLOCK, fp);
            ms_hash_mem(buf, (uint32_t)n, m->bhash[m->nblocks + b]);
        }
        free(buf); fclose(fp);
        m->nblocks += f.nblocks;
    }
    ms_file_t* nf = realloc(m->files, sizeof(ms_file_t) * (size_t)(m->nfiles + 1));
    if (!nf) return -1;
    m->files = nf;
    m->files[m->nfiles++] = f;
    m->total_bytes += f.size;
    return 0;
}

static int cmp_str(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

static int walk(ms_manifest_t* m, const char* full, const char* rel, int depth) {
    if (depth > 12) return 0;
    DIR* d = opendir(full);
    if (!d) return -1;
    char* names[4096];
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && n < 4096) {
        if (e->d_name[0] == '.') continue;          /* ., .., ocultos */
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], cmp_str);   /* mismo orden en todas las máquinas */
    int rc = 0;
    for (int i = 0; i < n; i++) {
        char f2[1024], r2[1024];
        snprintf(f2, sizeof f2, "%s/%s", full, names[i]);
        snprintf(r2, sizeof r2, "%s/%s", rel, names[i]);
        struct stat st;
        if (stat(f2, &st) == 0) {
            if (S_ISDIR(st.st_mode)) { if (walk(m, f2, r2, depth + 1) != 0) rc = -1; }
            else if (S_ISREG(st.st_mode)) { if (add_file(m, f2, r2) != 0) rc = -1; }
        }
        free(names[i]);
    }
    return rc;
}

int ms_manifest_add_mod(ms_manifest_t* m, const char* root, const char* name) {
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", root, name);
    struct stat st;
    if (stat(full, &st) != 0) return -1;
    char (*nm)[96] = realloc(m->mods, sizeof(*m->mods) * (size_t)(m->nmods + 1));
    if (!nm) return -1;
    m->mods = nm;
    snprintf(m->mods[m->nmods++], 96, "%s", name);
    if (S_ISDIR(st.st_mode)) return walk(m, full, name, 0);
    return add_file(m, full, name);
}

static void put16(uint8_t** p, uint16_t v) { memcpy(*p, &v, 2); *p += 2; }
static void put32(uint8_t** p, uint32_t v) { memcpy(*p, &v, 4); *p += 4; }

void ms_manifest_finish(ms_manifest_t* m) {
    size_t cap = 8 + (size_t)m->nmods * 100 + (size_t)m->nfiles * (sizeof(ms_file_t) + 8) + (size_t)m->nblocks * 16;
    free(m->blob);
    m->blob = malloc(cap);
    uint8_t* p = m->blob;
    memcpy(p, "MSM2", 4); p += 4;
    put16(&p, (uint16_t)m->nmods);
    for (int i = 0; i < m->nmods; i++) { uint8_t l = (uint8_t)strlen(m->mods[i]); *p++ = l; memcpy(p, m->mods[i], l); p += l; }
    put16(&p, (uint16_t)m->nfiles);
    for (int i = 0; i < m->nfiles; i++) {
        uint8_t l = (uint8_t)strlen(m->files[i].path);
        *p++ = l; memcpy(p, m->files[i].path, l); p += l;
        put32(&p, m->files[i].size);
        memcpy(p, m->files[i].hash, 16); p += 16;
        memcpy(p, m->bhash[m->files[i].block0], 16 * (size_t)m->files[i].nblocks); p += 16 * m->files[i].nblocks;
    }
    m->blob_len = (int)(p - m->blob);
    ms_hash_mem(m->blob, (uint32_t)m->blob_len, m->id);
    m->has_sig = 0;
}

int ms_manifest_parse(ms_manifest_t* m, const uint8_t* d, int len) {
    ms_manifest_init(m);
    const uint8_t* p = d, * e = d + len;
#define NEED(k) do { if (e - p < (k)) goto bad; } while (0)
    NEED(6);
    if (memcmp(p, "MSM2", 4) != 0) goto bad;
    p += 4;
    uint16_t nm; memcpy(&nm, p, 2); p += 2;
    m->mods = calloc(nm ? nm : 1, sizeof(*m->mods));
    for (int i = 0; i < nm; i++) { NEED(1); uint8_t l = *p++; NEED(l); if (l >= 96) goto bad; memcpy(m->mods[i], p, l); m->mods[i][l] = 0; p += l; m->nmods++; }
    NEED(2);
    uint16_t nf; memcpy(&nf, p, 2); p += 2;
    m->files = calloc(nf ? nf : 1, sizeof(ms_file_t));
    for (int i = 0; i < nf; i++) {
        NEED(1); uint8_t l = *p++; NEED(l + 20); if (l >= sizeof m->files[i].path) goto bad;
        memcpy(m->files[i].path, p, l); m->files[i].path[l] = 0; p += l;
        memcpy(&m->files[i].size, p, 4); p += 4;
        memcpy(m->files[i].hash, p, 16); p += 16;
        uint32_t nb = (m->files[i].size + MS_BLOCK - 1) / MS_BLOCK;
        if (e - p < (long)(16 * (size_t)nb)) goto bad;
        m->files[i].block0 = m->nblocks; m->files[i].nblocks = nb;
        if (nb) {
            uint8_t (*bh)[16] = realloc(m->bhash, 16 * (size_t)(m->nblocks + nb));
            if (!bh) goto bad;
            m->bhash = bh;
            memcpy(m->bhash[m->nblocks], p, 16 * (size_t)nb); p += 16 * nb;
            m->nblocks += nb;
        }
        if (strstr(m->files[i].path, "..") || m->files[i].path[0] == '/' || m->files[i].path[0] == '\\' || strchr(m->files[i].path, ':')) goto bad;   /* nada fuera de la carpeta */
        m->total_bytes += m->files[i].size;
        m->nfiles++;
    }
    int body = (int)(p - d);
    ms_hash_mem(d, (uint32_t)body, m->id);         /* el id no incluye la firma */
    if (e - p >= 100 && memcmp(p, MS_SIG_TAG, 4) == 0) {
        uint8_t msg[20]; sig_msg(m->id, msg);
        if (ms_check(p + 36, p + 4, msg, 20) != 0) goto bad;   /* firma que no vale: se descarta */
        memcpy(m->pub, p + 4, 32); memcpy(m->sig, p + 36, 64); m->has_sig = 1;
        len = body + 100;
    } else len = body;
    m->blob = malloc((size_t)len);
    memcpy(m->blob, d, (size_t)len);
    m->blob_len = len;
    return 0;
bad:
    ms_manifest_free(m);
    return -1;
#undef NEED
}

/* ------------------------------------------------------------------ semilla */
typedef struct { uint16_t file; uint32_t off; uint16_t len; } ms_req_t;
typedef struct { uint16_t id; double last; ms_req_t* q; int qh, qn, qcap; } ms_peer_t;
#define MS_MAX_ACTIVE 32
#define MS_MAX_WAIT 1024
struct ms_seeder {
    const ms_manifest_t* m;
    ms_io_t io;
    int max_active;
    double rate, tokens, last;
    ms_peer_t act[MS_MAX_ACTIVE];
    int nact, rr;
    struct { uint16_t id; double last; } wait[MS_MAX_WAIT];
    int nwait;
    ms_seeder_stats_t st;
    int (*has_block)(void* user, uint32_t block);
    void* has_user;
};
void ms_seeder_set_partial(ms_seeder_t* s, int (*has_block)(void* user, uint32_t block), void* user) { s->has_block = has_block; s->has_user = user; }

static uint32_t block_of(const ms_manifest_t* m, int file, uint32_t off) { return m->files[file].block0 + off / MS_BLOCK; }

/* MS_HAVE: los bloques que se tienen (en tandas de hasta 8000) */
static void send_have(ms_seeder_t* s, uint16_t to) {
    uint8_t b[16 + 1000];
    b[0] = MS_HAVE; memcpy(b + 1, s->m->id, 8);
    if (!s->has_block) {
        uint32_t z = 0; uint16_t all = 0xFFFF;
        memcpy(b + 9, &z, 4); memcpy(b + 13, &all, 2);
        s->io.send(s->io.user, to, b, 15);
        return;
    }
    for (uint32_t start = 0; start < s->m->nblocks; start += 8000) {
        uint16_t n = (uint16_t)(s->m->nblocks - start < 8000 ? s->m->nblocks - start : 8000);
        memcpy(b + 9, &start, 4); memcpy(b + 13, &n, 2);
        memset(b + 15, 0, (size_t)(n + 7) / 8);
        for (uint32_t k = 0; k < n; k++) if (s->has_block(s->has_user, start + k)) b[15 + k / 8] |= (uint8_t)(1 << (k % 8));
        s->io.send(s->io.user, to, b, 15 + (n + 7) / 8);
    }
}

ms_seeder_t* ms_seeder_new(const ms_manifest_t* m, const ms_io_t* io, int max_active, int chunks_per_s) {
    ms_seeder_t* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->m = m; s->io = *io;
    s->max_active = max_active < 1 ? 1 : max_active > MS_MAX_ACTIVE ? MS_MAX_ACTIVE : max_active;
    s->rate = chunks_per_s > 0 ? chunks_per_s : 200;
    return s;
}
void ms_seeder_free(ms_seeder_t* s) { if (!s) return; for (int i = 0; i < s->nact; i++) free(s->act[i].q); free(s); }

static void send_manifest(ms_seeder_t* s, uint16_t to) {
    int parts = (s->m->blob_len + MS_PART - 1) / MS_PART;
    if (parts < 1) parts = 1;
    if (parts > 255) return;
    uint8_t b[MS_PART + 3];
    for (int k = 0; k < parts; k++) {
        int n = s->m->blob_len - k * MS_PART; if (n > MS_PART) n = MS_PART;
        b[0] = MS_MANIFEST; b[1] = (uint8_t)k; b[2] = (uint8_t)parts;
        memcpy(b + 3, s->m->blob + k * MS_PART, (size_t)n);
        s->io.send(s->io.user, to, b, n + 3);
    }
    s->st.pedidos_manifiesto++;
}

static void send_small(ms_seeder_t* s, uint16_t to, uint8_t type, int pos) {
    uint8_t b[11];
    b[0] = type; memcpy(b + 1, s->m->id, 8);
    uint16_t p = (uint16_t)pos; memcpy(b + 9, &p, 2);
    s->io.send(s->io.user, to, b, type == MS_BUSY ? 11 : 9);
}

static int find_act(ms_seeder_t* s, uint16_t id) { for (int i = 0; i < s->nact; i++) if (s->act[i].id == id) return i; return -1; }
static int find_wait(ms_seeder_t* s, uint16_t id) { for (int i = 0; i < s->nwait; i++) if (s->wait[i].id == id) return i; return -1; }

static void push_req(ms_peer_t* p, ms_req_t r) {
    if (p->qn == p->qcap) {
        int nc = p->qcap ? p->qcap * 2 : 64;
        ms_req_t* nq = malloc(sizeof(ms_req_t) * (size_t)nc);
        if (!nq) return;
        for (int i = 0; i < p->qn; i++) nq[i] = p->q[(p->qh + i) % p->qcap];
        free(p->q); p->q = nq; p->qh = 0; p->qcap = nc;
    }
    p->q[(p->qh + p->qn) % p->qcap] = r;
    p->qn++;
}

bool ms_seeder_on_msg(ms_seeder_t* s, uint16_t from, const uint8_t* d, int len, double now) {
    if (len < 1 || d[0] < MS_MSG_FIRST || d[0] > MS_MSG_LAST) return false;
    if (d[0] == MS_MANIFEST_REQ) { send_manifest(s, from); return true; }
    if (d[0] == MS_HAVE_REQ) {
        if (len >= 9 && memcmp(d + 1, s->m->id, 8) == 0) send_have(s, from);
        else if (len >= 9) send_small(s, from, MS_NOPE, 0);
        return true;
    }
    if (d[0] != MS_CHUNK_REQ) return false;
    if (len < 9) return true;
    if (memcmp(d + 1, s->m->id, 8) != 0) { send_small(s, from, MS_NOPE, 0); return true; }
    int i = find_act(s, from);
    if (i < 0) {
        if (s->nact < s->max_active) {
            int w = find_wait(s, from);
            /* el primero de la lista de espera tiene prioridad sobre uno nuevo */
            if (s->nwait > 0 && w != 0) {
                if (w < 0 && s->nwait < MS_MAX_WAIT) { s->wait[s->nwait].id = from; s->wait[s->nwait].last = now; s->nwait++; w = s->nwait - 1; }
                if (w >= 0) s->wait[w].last = now;
                send_small(s, from, MS_BUSY, w + 1); s->st.en_espera_avisos++;
                return true;
            }
            if (w == 0) { memmove(&s->wait[0], &s->wait[1], sizeof(s->wait[0]) * (size_t)(s->nwait - 1)); s->nwait--; }
            memset(&s->act[s->nact], 0, sizeof s->act[0]);
            s->act[s->nact].id = from;
            i = s->nact++;
        } else {
            int w = find_wait(s, from);
            if (w < 0 && s->nwait < MS_MAX_WAIT) { s->wait[s->nwait].id = from; w = s->nwait++; }
            if (w >= 0) { s->wait[w].last = now; send_small(s, from, MS_BUSY, w + 1); s->st.en_espera_avisos++; }
            return true;
        }
    }
    ms_peer_t* p = &s->act[i];
    p->last = now;
    for (int o = 9; o + 8 <= len; o += 8) {
        ms_req_t r;
        memcpy(&r.file, d + o, 2); memcpy(&r.off, d + o + 2, 4); memcpy(&r.len, d + o + 6, 2);
        if (r.file >= s->m->nfiles || r.len == 0 || r.len > MS_CHUNK_DATA || r.off >= s->m->files[r.file].size) continue;
        if (s->has_block && !s->has_block(s->has_user, block_of(s->m, r.file, r.off))) continue;   /* no lo tengo (todavía) */
        if (p->qn < 4096) push_req(p, r);
    }
    return true;
}

void ms_seeder_update(ms_seeder_t* s, double now) {
    double dt = s->last > 0 ? now - s->last : 0;
    s->last = now;
    s->tokens += s->rate * dt / 1000.0;
    if (s->tokens > s->rate / 5 + 1) s->tokens = s->rate / 5 + 1;
    /* fuera los que dejaron de pedir; entran los de la lista de espera */
    for (int i = 0; i < s->nact; i++) {
        if (s->act[i].qn == 0 && now - s->act[i].last > 2500) {
            free(s->act[i].q);
            s->act[i] = s->act[--s->nact];
            i--;
        }
    }
    for (int i = 0; i < s->nwait; i++) if (now - s->wait[i].last > 8000) { memmove(&s->wait[i], &s->wait[i + 1], sizeof(s->wait[0]) * (size_t)(s->nwait - i - 1)); s->nwait--; i--; }
    uint8_t b[13 + MS_CHUNK_DATA];
    int idle = 0;
    while (s->tokens >= 1 && s->nact > 0 && idle < s->nact) {
        s->rr = (s->rr + 1) % s->nact;
        ms_peer_t* p = &s->act[s->rr];
        if (p->qn == 0) { idle++; continue; }
        idle = 0;
        ms_req_t r = p->q[p->qh];
        uint32_t fsz = s->m->files[r.file].size;
        int n = r.len;
        if (r.off + (uint32_t)n > fsz) n = (int)(fsz - r.off);
        b[0] = MS_CHUNK; memcpy(b + 1, s->m->id, 8); memcpy(b + 9, &r.file, 2); memcpy(b + 11, &r.off, 4);
        int got = s->io.read(s->io.user, r.file, r.off, b + 15, n);
        if (got > 0 && s->io.send(s->io.user, p->id, b, 15 + got) != 0) break;   /* la red no da más: después */
        p->qh = (p->qh + 1) % p->qcap; p->qn--;
        s->tokens -= 1;
        if (got > 0) { s->st.pedazos++; s->st.bytes += (unsigned long long)got; }
    }
    s->st.atendiendo = s->nact;
    s->st.en_espera = s->nwait;
}

void ms_seeder_stats(const ms_seeder_t* s, ms_seeder_stats_t* st) { *st = s->st; }

bool ms_reply_not_ready(const ms_io_t* io, uint16_t to, const uint8_t* d, int len) {
    if (len < 1 || (d[0] != MS_CHUNK_REQ && d[0] != MS_MANIFEST_REQ && d[0] != MS_HAVE_REQ)) return false;
    if ((d[0] == MS_CHUNK_REQ || d[0] == MS_HAVE_REQ) && len >= 9) {
        uint8_t b[11];
        b[0] = MS_BUSY; memcpy(b + 1, d + 1, 8);
        uint16_t p = MS_NOT_READY; memcpy(b + 9, &p, 2);
        io->send(io->user, to, b, 11);
    }
    return true;
}

/* ------------------------------------------------------------------ descarga */
#define CPB (MS_BLOCK / MS_CHUNK_DATA)   /* pedazos por bloque */
typedef struct { uint16_t id; bool alive, answered, nope, full; double busy_until, dead_at, have_at, have_req; int inflight, fails, queue_pos, good, bad; uint8_t* bits; } ms_src_t;
typedef struct { uint8_t state; uint8_t src; double t; } ms_chunk_t;   /* 0 falta, 1 pedido, 2 está */
typedef struct { int file; uint8_t* data; int nchunks, got; ms_chunk_t* ch; uint16_t* bgot; uint8_t* bok; int16_t* bpin; uint8_t* bbad; int nb, bdone; bool done; } ms_need_t;
#define MS_MAX_SRC 64
struct ms_dl {
    ms_io_t io;
    ms_dl_status_t st;
    uint16_t mfrom;
    double mreq;
    uint8_t* mbuf; int mlen; uint8_t mparts, mgot; uint8_t mseen[256];
    ms_manifest_t man; bool have_man;
    ms_need_t* need; int nneed;
    int* need_of;            /* por archivo: índice en need o -1 */
    uint8_t* have;           /* por bloque global: 1 si ya está (verificado) */
    uint8_t* file_ok;        /* por archivo: completo en disco/memoria del usuario */
    ms_src_t src[MS_MAX_SRC]; int nsrc;
    int ci, ck;
    bool serving; int srv_active, srv_rate;
    ms_seeder_t* sd;
};

ms_dl_t* ms_dl_new(const ms_io_t* io) {
    ms_dl_t* d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->io = *io;
    return d;
}
void ms_dl_free(ms_dl_t* d) {
    if (!d) return;
    for (int i = 0; i < d->nneed; i++) { ms_need_t* n = &d->need[i]; free(n->data); free(n->ch); free(n->bgot); free(n->bok); free(n->bpin); free(n->bbad); }
    for (int i = 0; i < d->nsrc; i++) free(d->src[i].bits);
    free(d->need); free(d->need_of); free(d->have); free(d->file_ok); free(d->mbuf);
    ms_seeder_free(d->sd);
    if (d->have_man) ms_manifest_free(&d->man);
    free(d);
}
void ms_dl_add_source(ms_dl_t* d, uint16_t id) {
    for (int i = 0; i < d->nsrc; i++) if (d->src[i].id == id) return;
    if (d->nsrc >= MS_MAX_SRC) return;
    memset(&d->src[d->nsrc], 0, sizeof d->src[0]);
    d->src[d->nsrc].id = id; d->src[d->nsrc].alive = true;
    d->nsrc++;
}
void ms_dl_start(ms_dl_t* d, uint16_t from, double now) {
    d->mfrom = from; d->st.state = MS_DL_MANIFEST; d->st.started = now; d->mreq = 0;
    ms_dl_add_source(d, from);
}
void ms_dl_set_serving(ms_dl_t* d, bool on, int max_active, int chunks_per_s) { d->serving = on; d->srv_active = max_active; d->srv_rate = chunks_per_s; }
const ms_manifest_t* ms_dl_manifest(const ms_dl_t* d) { return d->have_man ? &d->man : NULL; }

/* repartir lo que ya hay: lo leído sale del archivo del usuario si está completo o de lo bajado */
static int dl_io_send(void* u, uint16_t to, const uint8_t* b, int n) { ms_dl_t* d = u; return d->io.send(d->io.user, to, b, n); }
static int dl_io_read(void* u, int file, uint32_t off, uint8_t* buf, int len) {
    ms_dl_t* d = u;
    if (d->file_ok[file]) return d->io.read ? d->io.read(d->io.user, file, off, buf, len) : -1;
    int ni = d->need_of[file];
    if (ni < 0 || !d->need[ni].data) return -1;
    memcpy(buf, d->need[ni].data + off, (size_t)len);
    return len;
}
static int dl_has_block(void* u, uint32_t b) { ms_dl_t* d = u; return b < d->man.nblocks && d->have[b]; }

static void dl_plan(ms_dl_t* d, double now) {
    d->st.files_total = d->man.nfiles;
    int nf = d->man.nfiles ? d->man.nfiles : 1;
    d->need = calloc((size_t)nf, sizeof(ms_need_t));
    d->need_of = malloc(sizeof(int) * (size_t)nf);
    d->file_ok = calloc((size_t)nf, 1);
    d->have = calloc(d->man.nblocks ? d->man.nblocks : 1, 1);
    for (int i = 0; i < d->man.nfiles; i++) {
        const ms_file_t* f = &d->man.files[i];
        d->need_of[i] = -1;
        if (d->io.have(d->io.user, f)) {
            d->file_ok[i] = 1;
            for (uint32_t b = 0; b < f->nblocks; b++) d->have[f->block0 + b] = 1;
            continue;
        }
        ms_need_t* n = &d->need[d->nneed];
        d->need_of[i] = d->nneed++;
        n->file = i;
        n->nchunks = (int)((f->size + MS_CHUNK_DATA - 1) / MS_CHUNK_DATA);
        n->nb = (int)f->nblocks;
        n->data = malloc(f->size ? f->size : 1);
        n->ch = calloc((size_t)(n->nchunks ? n->nchunks : 1), sizeof(ms_chunk_t));
        n->bgot = calloc((size_t)(n->nb ? n->nb : 1), sizeof(uint16_t));
        n->bok = calloc((size_t)(n->nb ? n->nb : 1), 1);
        n->bpin = malloc(sizeof(int16_t) * (size_t)(n->nb ? n->nb : 1));
        for (int b = 0; b < n->nb; b++) n->bpin[b] = -1;
        n->bbad = calloc((size_t)(n->nb ? n->nb : 1), 1);
        d->st.bytes_needed += f->size;
        if (n->nchunks == 0) {   /* archivo vacío */
            uint8_t h[16]; ms_hash_mem(n->data, 0, h);
            n->done = memcmp(h, f->hash, 16) == 0 && d->io.write(d->io.user, f, n->data, 0) == 0;
            if (n->done) { d->st.files_done++; d->file_ok[i] = 1; }
        }
    }
    d->st.files_needed = d->nneed;
    /* cada uno empieza por un lugar distinto: así los que van por la mitad tienen bloques
       distintos para repartirse entre ellos */
    { uint64_t r = (uint64_t)(uintptr_t)d ^ (uint64_t)(now * 1000.0); r ^= r >> 17; r *= 0x9E3779B97F4A7C15ull;
      if (d->nneed > 0) { d->ci = (int)(r % (uint64_t)d->nneed); d->ck = d->need[d->ci].nchunks ? (int)((r >> 32) % (uint64_t)d->need[d->ci].nchunks) / CPB * CPB : 0; } }
    d->st.state = d->st.files_done == d->nneed ? MS_DL_DONE : MS_DL_FILES;
    if (d->st.state == MS_DL_DONE) d->st.finished = now;
}

static int src_index(ms_dl_t* d, uint16_t id) { for (int i = 0; i < d->nsrc; i++) if (d->src[i].id == id) return i; return -1; }
static bool src_has(const ms_dl_t* d, const ms_src_t* s, uint32_t gb) { return s->full || (s->bits && (s->bits[gb / 8] >> (gb % 8)) & 1); }

static void dl_block_bad(ms_dl_t* d, ms_need_t* n, int b) {
    /* no se sabe qué pedazo vino mal: el bloque se pide entero a una sola semilla; si así también
       llega mal, esa semilla se descarta (tiene otra versión o manda basura) */
    d->st.blocks_bad++;
    int c0 = b * CPB, c1 = c0 + CPB < n->nchunks ? c0 + CPB : n->nchunks;
    if (n->bpin[b] >= 0) { ms_src_t* s = &d->src[n->bpin[b]]; s->alive = false; s->nope = true; }
    else {
        /* sospecha: las que mandaron pedazos de bloques malos mucho más que de buenos, afuera */
        bool seen[MS_MAX_SRC] = { false };
        for (int c = c0; c < c1; c++) if (n->ch[c].state == 2) seen[n->ch[c].src] = true;
        for (int si = 0; si < d->nsrc; si++) if (seen[si] && ++d->src[si].bad >= 6 && d->src[si].bad > 2 * d->src[si].good) { d->src[si].alive = false; d->src[si].nope = true; }
    }
    for (int c = c0; c < c1; c++) { if (n->ch[c].state == 2) { n->got--; } n->ch[c].state = 0; }
    uint32_t off = (uint32_t)b * MS_BLOCK, sz = d->man.files[n->file].size;
    d->st.bytes_got -= (sz - off < MS_BLOCK ? sz - off : MS_BLOCK);
    n->bgot[b] = 0;
    uint32_t gb = d->man.files[n->file].block0 + (uint32_t)b;
    int start = n->bpin[b] >= 0 ? n->bpin[b] + 1 : 0, pick = -1;
    for (int k = 0; k < d->nsrc; k++) { int si = (start + k) % d->nsrc; if (d->src[si].alive && !d->src[si].nope && src_has(d, &d->src[si], gb)) { pick = si; break; } }
    n->bpin[b] = (int16_t)pick;
    if (++n->bbad[b] >= 6) { d->st.state = MS_DL_FAILED; snprintf(d->st.error, sizeof d->st.error, "%.90s: un bloque llega distinto de todas las semillas", d->man.files[n->file].path); }
}

static void dl_chunk_arrived(ms_dl_t* d, ms_need_t* n, int c, double now) {
    const ms_file_t* f = &d->man.files[n->file];
    int b = c / CPB;
    int inblock = (b + 1) * CPB <= n->nchunks ? CPB : n->nchunks - b * CPB;
    if (++n->bgot[b] < inblock) return;
    uint32_t off = (uint32_t)b * MS_BLOCK, len = f->size - off < MS_BLOCK ? f->size - off : MS_BLOCK;
    uint8_t h[16];
    ms_hash_mem(n->data + off, len, h);
    if (memcmp(h, d->man.bhash[f->block0 + (uint32_t)b], 16) != 0) { dl_block_bad(d, n, b); return; }
    n->bok[b] = 1;
    d->have[f->block0 + (uint32_t)b] = 1;   /* desde ya se puede repartir */
    { bool seen[MS_MAX_SRC] = { false }; int c0 = b * CPB, c1 = c0 + CPB < n->nchunks ? c0 + CPB : n->nchunks;
      for (int k = c0; k < c1; k++) seen[n->ch[k].src] = true;
      for (int si = 0; si < d->nsrc; si++) if (seen[si]) d->src[si].good++; }
    if (++n->bdone < n->nb) return;
    ms_hash_mem(n->data, f->size, h);
    if (memcmp(h, f->hash, 16) != 0 || d->io.write(d->io.user, f, n->data, f->size) != 0) {
        d->st.files_bad++;
        d->st.state = MS_DL_FAILED; snprintf(d->st.error, sizeof d->st.error, "%.90s no se pudo guardar", f->path);
        return;
    }
    n->done = true;
    d->file_ok[n->file] = 1;
    if (!d->io.read) { /* sin forma de leerlo de nuevo: se queda en memoria para repartir */ }
    else { free(n->data); n->data = NULL; }
    d->st.files_done++;
    if (d->st.files_done == d->nneed) { d->st.state = MS_DL_DONE; d->st.finished = now; }
}

bool ms_dl_on_msg(ms_dl_t* d, uint16_t from, const uint8_t* m, int len, double now) {
    if (len < 1 || m[0] < MS_MSG_FIRST || m[0] > MS_MSG_LAST) return false;
    /* pedidos de otros: se reparte lo que hay, o "todavía no" */
    if (m[0] == MS_MANIFEST_REQ || m[0] == MS_CHUNK_REQ || m[0] == MS_HAVE_REQ) {
        if (d->sd && ms_seeder_on_msg(d->sd, from, m, len, now)) return true;
        return ms_reply_not_ready(&d->io, from, m, len);
    }
    int si = src_index(d, from);
    ms_src_t* s = si >= 0 ? &d->src[si] : NULL;
    if (m[0] == MS_MANIFEST) {
        if (d->have_man || from != d->mfrom || len < 3) return true;
        uint8_t k = m[1], parts = m[2];
        if (parts == 0) return true;
        if (!d->mbuf || d->mparts != parts) { free(d->mbuf); d->mbuf = calloc(parts, MS_PART); d->mparts = parts; d->mgot = 0; memset(d->mseen, 0, sizeof d->mseen); d->mlen = 0; }
        if (k >= parts || d->mseen[k] || len - 3 > MS_PART) return true;
        memcpy(d->mbuf + k * MS_PART, m + 3, (size_t)(len - 3));
        d->mseen[k] = 1; d->mgot++;
        if (k == parts - 1) d->mlen = k * MS_PART + (len - 3);
        if (d->mgot == parts && d->mlen > 0) {
            if (ms_manifest_parse(&d->man, d->mbuf, d->mlen) == 0) { d->have_man = true; dl_plan(d, now); }
            else { d->st.state = MS_DL_FAILED; snprintf(d->st.error, sizeof d->st.error, "manifiesto dañado"); }
        }
        return true;
    }
    if (!d->have_man || len < 9 || memcmp(m + 1, d->man.id, 8) != 0 || !s) return true;
    s->fails = 0;
    if (m[0] == MS_HAVE) {
        if (len < 15) return true;
        uint32_t start; uint16_t n; memcpy(&start, m + 9, 4); memcpy(&n, m + 13, 2);
        s->answered = true; s->have_at = now;
        if (n == 0xFFFF) { s->full = true; return true; }
        if (!s->bits) s->bits = calloc((d->man.nblocks + 7) / 8 + 1, 1);
        for (uint32_t k = 0; k < n && 15 + (int)(k / 8) < len && start + k < d->man.nblocks; k++)
            if ((m[15 + k / 8] >> (k % 8)) & 1) s->bits[(start + k) / 8] |= (uint8_t)(1 << ((start + k) % 8));
        return true;
    }
    if (m[0] == MS_NOPE) { s->alive = false; s->nope = true; }
    if (m[0] == MS_BUSY || m[0] == MS_NOPE) {
        if (m[0] == MS_BUSY && len >= 11) {
            uint16_t pos; memcpy(&pos, m + 9, 2);
            if (pos == MS_NOT_READY) { s->queue_pos = 0; s->busy_until = now + 3000; }   /* todavía no tiene nada: más tarde */
            else { s->queue_pos = pos; s->busy_until = now + 1500; s->answered = true; }
        }
        for (int i = 0; i < d->nneed; i++) for (int c = 0; c < d->need[i].nchunks; c++)
            if (d->need[i].ch[c].state == 1 && d->need[i].ch[c].src == si) d->need[i].ch[c].state = 0;
        s->inflight = 0;
        return true;
    }
    if (m[0] != MS_CHUNK || len < 15) return true;
    s->answered = true; s->queue_pos = 0;
    uint16_t file; uint32_t off;
    memcpy(&file, m + 9, 2); memcpy(&off, m + 11, 4);
    int n = len - 15;
    if (file >= d->man.nfiles || d->need_of[file] < 0) return true;
    ms_need_t* nd = &d->need[d->need_of[file]];
    if (nd->done) return true;
    const ms_file_t* f = &d->man.files[file];
    int c = (int)(off / MS_CHUNK_DATA);
    if (off % MS_CHUNK_DATA || c >= nd->nchunks || off + (uint32_t)n > f->size) return true;
    if (nd->ch[c].state == 1 && nd->ch[c].src == (uint8_t)si && s->inflight > 0) s->inflight--;
    if (nd->ch[c].state == 2 || nd->bok[c / CPB]) return true;
    /* un bloque fijado a una semilla solo acepta lo de esa semilla */
    if (nd->bpin[c / CPB] >= 0 && nd->bpin[c / CPB] != si) return true;
    memcpy(nd->data + off, m + 15, (size_t)n);
    nd->ch[c].state = 2;
    nd->got++;
    d->st.bytes_got += (uint64_t)n;
    dl_chunk_arrived(d, nd, c, now);
    return true;
}

void ms_dl_update(ms_dl_t* d, double now) {
    if (d->sd) {
        ms_seeder_update(d->sd, now);
        ms_seeder_stats_t ss; ms_seeder_stats(d->sd, &ss);
        d->st.bytes_served = ss.bytes;
    }
    if (d->st.state == MS_DL_MANIFEST) {
        if (now - d->mreq > 2000) { uint8_t b = MS_MANIFEST_REQ; d->io.send(d->io.user, d->mfrom, &b, 1); d->mreq = now; }
        return;
    }
    if (d->serving && d->have_man && !d->sd) {
        ms_io_t sio = { d, dl_io_send, dl_io_read, NULL, NULL };
        d->sd = ms_seeder_new(&d->man, &sio, d->srv_active, d->srv_rate);
        if (d->sd) ms_seeder_set_partial(d->sd, dl_has_block, d);
    }
    if (d->st.state != MS_DL_FILES) return;
    /* pedidos vencidos: vuelven a la lista; la semilla que no contesta se descarta */
    for (int i = 0; i < d->nneed; i++) {
        ms_need_t* n = &d->need[i];
        if (n->done) continue;
        for (int c = 0; c < n->nchunks; c++) {
            ms_chunk_t* ch = &n->ch[c];
            if (ch->state == 1 && now - ch->t > MS_TIMEOUT) {
                ch->state = 0;
                ms_src_t* s = &d->src[ch->src];
                if (s->inflight > 0) s->inflight--;
                if (++s->fails >= (s->answered ? 40 : 3)) { s->alive = false; s->dead_at = now; }
            }
        }
    }
    int alive = 0, busy = 0, best = 0, total = 0;
    for (int i = 0; i < d->nneed; i++) total += d->need[i].nchunks;
    for (int si = 0; si < d->nsrc; si++) {
        ms_src_t* s = &d->src[si];
        /* las que no contestaban se vuelven a probar cada 15 s */
        if (!s->alive && !s->nope && now - s->dead_at > 15000) { s->alive = true; s->fails = 0; }
        if (!s->alive) continue;
        alive++;
        if (now < s->busy_until) { busy++; if (s->queue_pos && (!best || s->queue_pos < best)) best = s->queue_pos; continue; }
        /* qué tiene: a las parciales se les vuelve a preguntar cada 2 s */
        if (!s->full && (now - s->have_req > 2000)) {
            uint8_t b[9]; b[0] = MS_HAVE_REQ; memcpy(b + 1, d->man.id, 8);
            if (d->io.send(d->io.user, s->id, b, 9) == 0) s->have_req = now;
        }
        if (!s->full && !s->bits) continue;
        int window = MS_WINDOW;
        if (s->inflight >= window) continue;
        uint8_t b[9 + MS_PER_REQ * 8];
        b[0] = MS_CHUNK_REQ; memcpy(b + 1, d->man.id, 8);
        int nb = 0;
        for (int scanned = 0; scanned < total && s->inflight < window && nb < MS_PER_REQ; scanned++) {
            if (d->ci >= d->nneed) { d->ci = 0; d->ck = 0; }
            ms_need_t* n = &d->need[d->ci];
            int k = d->ck;
            if (++d->ck >= n->nchunks) { d->ck = 0; d->ci++; }
            if (n->done || k >= n->nchunks || n->ch[k].state != 0) continue;
            int bl = k / CPB;
            if (n->bok[bl]) continue;
            if (!src_has(d, s, d->man.files[n->file].block0 + (uint32_t)bl)) continue;
            if (n->bpin[bl] >= 0 && n->bpin[bl] != si) {
                if (!d->src[n->bpin[bl]].alive) n->bpin[bl] = -1; else continue;
            }
            uint16_t file = (uint16_t)n->file; uint32_t off = (uint32_t)k * MS_CHUNK_DATA;
            uint32_t rem = d->man.files[n->file].size - off;
            uint16_t ln = (uint16_t)(rem < MS_CHUNK_DATA ? rem : MS_CHUNK_DATA);
            memcpy(b + 9 + nb * 8, &file, 2); memcpy(b + 9 + nb * 8 + 2, &off, 4); memcpy(b + 9 + nb * 8 + 6, &ln, 2);
            nb++;
            n->ch[k].state = 1; n->ch[k].src = (uint8_t)si; n->ch[k].t = now;
            s->inflight++;
        }
        if (nb > 0 && d->io.send(d->io.user, s->id, b, 9 + nb * 8) != 0) {
            for (int i = 0; i < d->nneed; i++) for (int c = 0; c < d->need[i].nchunks; c++)
                if (d->need[i].ch[c].state == 1 && d->need[i].ch[c].src == si && d->need[i].ch[c].t == now) { d->need[i].ch[c].state = 0; s->inflight--; }
        }
    }
    d->st.sources_alive = alive; d->st.sources_busy = busy; d->st.queue_pos = (alive > 0 && busy == alive) ? best : 0;
    if (alive == 0 && now - d->st.started > 30000) { d->st.state = MS_DL_FAILED; snprintf(d->st.error, sizeof d->st.error, "ninguna semilla contesta"); }
}

void ms_dl_status(const ms_dl_t* d, ms_dl_status_t* st) { *st = d->st; }
