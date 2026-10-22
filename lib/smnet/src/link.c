/* Capa de enlace: ver link.h. */
#include <stdlib.h>
#include <string.h>
#include "link.h"
#include "../third_party/monocypher/monocypher.h"

#define PK_INIT   0xA1
#define PK_RESP   0xA2
#define PK_DATA   0xA3
#define PK_PUNCH  0xA4
#define PK_RESET  0xA5          /* "no tengo esa sesión": el otro rehace el handshake */

#define INIT_SIZE  160         /* v2: con identidad, marca de tiempo y firma (relleno: el RESP no amplifica) */
#define RESP_SIZE  153
#define LINK_VER   2
#define TS_SLOTS   4096           /* última marca de tiempo vista por identidad (anti-replay de INIT) */
#define DATA_HDR   13           /* tipo + sid + pn */
#define MAC        16
/* Al juntar mensajes en un datagrama no se pasa de SMNET_MTU en total; solo un mensaje grande que
 * va solo (con, a lo sumo, un ack) puede llegar a LINK_MAX_INNER. Un datagrama de ~1300 B ya se
 * fragmenta en algunos túneles. */
#define PACK_MAX   (SMNET_MTU - DATA_HDR - MAC)
#define ACK_ONLY   13

#define F_ACK      0x01
#define FR_UNREL   0
#define FR_REL     1

#define OOO_WIN    64
#define RELQ_CAP   512
#define PENDQ_CAP  4
#define SENT_RING  64
#define NBUCKET    1024
#define SLOT_BITS  11           /* LINK_MAX_SESS = 2048 */

enum { S_FREE = 0, S_INIT_SENT, S_EST };

typedef struct {
    uint16_t rseq;
    uint16_t len;
    uint8_t  tries;
    uint64_t pn_last;
    double   t_sent;
    uint8_t* data;
} relmsg_t;

typedef struct { uint16_t len; uint8_t* data; } pend_t;
typedef struct { uint64_t pn; double t; } sent_t;
typedef struct { uint8_t have; uint16_t len; uint8_t* data; } ooo_t;

typedef struct sess {
    int          state;
    int          slot, active_pos;
    int          hnext;
    smnet_addr_t addr;
    uint32_t     my_sid, their_sid;
    uint8_t      eph_sk[32], eph_pk[32], peer_pk[32];
    uint8_t      peer_id[32];            /* identidad Ed25519 del otro (verificada en el handshake) */
    uint16_t     tag;                    /* id de jugador verificado por la aplicación (PRESENT) */
    uint8_t      presented;              /* ya mandamos nuestra presentación por esta sesión */
    uint8_t      k_tx[32], k_rx[32];
    uint8_t      resp[RESP_SIZE];        /* para repetir el RESP si se perdió */
    int          is_responder;
    int          closing;                /* link_close diferido: se libera en link_update */
    int          work_pos;               /* posición en la lista de trabajo (-1: no está) */
    int          authed;                 /* ya recibió al menos un DATA auténtico */
    double       created, last_rx, last_tx, hs_next;
    int          hs_tries;
    uint64_t     init_ts;
    uint64_t     tx_pn;                  /* próximo a usar (desde 1) */
    uint64_t     rx_max, rx_bits;        /* ventana anti-replay = estado de acks */
    int          ack_need;
    double       ack_due;
    uint64_t     acked_max;              /* mayor pn nuestro que el otro confirmó */
    float        srtt, rttvar;
    sent_t       sent[SENT_RING];
    relmsg_t     relq[RELQ_CAP];
    int          relq_n;
    uint16_t     tx_rseq, rx_rseq;
    ooo_t        ooo[OOO_WIN];
    pend_t       pendq[PENDQ_CAP];
    int          pendq_n;
} sess_t;

struct link {
    link_cfg_t   cfg;
    link_out_fn  out;
    link_in_fn   in;
    void*        ctx;
    uint8_t      seed[32];
    uint64_t     rng_ctr;
    uint8_t      rng_buf[64];
    int          rng_pos;
    sess_t*      slots[LINK_MAX_SESS];
    int          active[LINK_MAX_SESS];
    int          nactive;
    int          work[LINK_MAX_SESS];    /* sesiones con algo pendiente (handshake, confiables, acks) */
    int          nwork;
    double       next_scan;              /* expiración: una vez por segundo */
    int          bucket[NBUCKET];
    double       now;
    link_stats_t st;
    sess_t*      busy;                   /* sesión en proceso de recepción: no se desaloja */
    uint8_t      pres[256];              /* presentación de la aplicación (PRESENT): primer confiable de cada sesión */
    int          pres_len;
    uint8_t      id_sk[64], id_pk[32];   /* identidad propia */
    struct { uint64_t key, ts; } seen_ts[TS_SLOTS];
    double       reset_window;
    int          resets_in_window;
    uint8_t      pkt[LINK_MAX_INNER + 128];
    uint8_t      plain[LINK_MAX_INNER + 128];
};

/* ------------------------------------------------------------ utilidades */

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint64_t get64(const uint8_t* p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

void link_cfg_default(link_cfg_t* c) {
    c->hs_retry_ms = 250;
    c->hs_tries = 16;
    c->idle_expire_ms = 30000;
    c->ack_delay_ms = 20;
    c->encrypt = 1;
}

void link_random(link_t* l, uint8_t* out, int n) {
    while (n > 0) {
        if (l->rng_pos >= 64) {
            uint8_t c[8];
            put64(c, l->rng_ctr++);
            crypto_blake2b_keyed(l->rng_buf, 64, l->seed, 32, c, 8);
            l->rng_pos = 0;
        }
        int k = 64 - l->rng_pos;
        if (k > n) k = n;
        memcpy(out, l->rng_buf + l->rng_pos, (size_t)k);
        l->rng_pos += k; out += k; n -= k;
    }
}

static unsigned addr_hash(const smnet_addr_t* a) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < a->len && i < (int)sizeof a->data; i++) { h ^= a->data[i]; h *= 16777619u; }
    return h & (NBUCKET - 1);
}

static sess_t* find_addr(const link_t* l, const smnet_addr_t* a) {
    for (int i = l->bucket[addr_hash(a)]; i >= 0; i = l->slots[i]->hnext)
        if (!l->slots[i]->closing && smnet_addr_eq(&l->slots[i]->addr, a)) return l->slots[i];
    return NULL;
}

static sess_t* find_sid(const link_t* l, uint32_t sid) {
    int slot = (int)(sid & (LINK_MAX_SESS - 1));
    sess_t* s = l->slots[slot];
    return s && s->my_sid == sid ? s : NULL;
}

static void hash_insert(link_t* l, sess_t* s) {
    unsigned h = addr_hash(&s->addr);
    s->hnext = l->bucket[h];
    l->bucket[h] = s->slot;
}

static void hash_remove(link_t* l, sess_t* s) {
    int* pp = &l->bucket[addr_hash(&s->addr)];
    while (*pp >= 0) {
        if (*pp == s->slot) { *pp = s->hnext; return; }
        pp = &l->slots[*pp]->hnext;
    }
}

static void work_add(link_t* l, sess_t* s) {
    if (s->work_pos >= 0) return;
    s->work_pos = l->nwork;
    l->work[l->nwork++] = s->slot;
}
static void work_del(link_t* l, sess_t* s) {
    int pos = s->work_pos;
    if (pos < 0) return;
    l->work[pos] = l->work[--l->nwork];
    if (pos < l->nwork) l->slots[l->work[pos]]->work_pos = pos;
    s->work_pos = -1;
}

static void clear_queues(sess_t* s) {
    for (int i = 0; i < s->relq_n; i++) free(s->relq[i].data);
    s->relq_n = 0;
    for (int i = 0; i < OOO_WIN; i++) { free(s->ooo[i].data); s->ooo[i].data = NULL; s->ooo[i].have = 0; }
    for (int i = 0; i < s->pendq_n; i++) free(s->pendq[i].data);
    s->pendq_n = 0;
}

static void sess_free(link_t* l, sess_t* s) {
    work_del(l, s);
    hash_remove(l, s);
    clear_queues(s);
    int pos = s->active_pos;
    l->active[pos] = l->active[--l->nactive];
    if (pos < l->nactive) l->slots[l->active[pos]]->active_pos = pos;
    l->slots[s->slot] = NULL;
    crypto_wipe(s, sizeof *s);
    free(s);
    l->st.sessions = l->nactive;
}

static relmsg_t* queue_rel(link_t* l, sess_t* s, const uint8_t* msg, int len);

/* La presentación va primera: el otro lado descarta lo nuestro hasta etiquetar la sesión. Se pone
 * al frente de lo pendiente en cada sesión nueva y cada vez que el otro perdió el estado. */
static void present_front(link_t* l, sess_t* s) {
    if (!l->pres_len) return;
    if (s->relq_n == RELQ_CAP) { free(s->relq[RELQ_CAP - 1].data); s->relq_n--; l->st.rel_dropped++; }
    if (!queue_rel(l, s, l->pres, l->pres_len)) return;
    relmsg_t m = s->relq[s->relq_n - 1];
    memmove(s->relq + 1, s->relq, sizeof(relmsg_t) * (size_t)(s->relq_n - 1));
    s->relq[0] = m;
    s->presented = 1;
}

static sess_t* sess_new(link_t* l, const smnet_addr_t* a) {
    if (l->nactive >= LINK_MAX_SESS) {
        /* lleno: se libera la sesión más vieja sin tráfico reciente */
        /* primero las que nunca recibieron DATA auténtico (un INIT con dirección falsa no
           desaloja sesiones vivas), y entre iguales la de menos tráfico reciente */
        sess_t* victim = NULL;
        for (int i = 0; i < l->nactive; i++) {
            sess_t* s = l->slots[l->active[i]];
            if (s == l->busy) continue;
            if (s->closing) { victim = s; break; }
            if (!victim || (!s->authed && victim->authed) ||
                (s->authed == victim->authed && s->last_rx < victim->last_rx)) victim = s;
        }
        if (!victim) return NULL;
        sess_free(l, victim);
    }
    int slot = -1;
    uint8_t r[4];
    link_random(l, r, 4);
    int start = (int)(get32(r) & (LINK_MAX_SESS - 1));
    for (int k = 0; k < LINK_MAX_SESS; k++) { int i = (start + k) & (LINK_MAX_SESS - 1); if (!l->slots[i]) { slot = i; break; } }
    sess_t* s = calloc(1, sizeof *s);
    if (!s || slot < 0) { free(s); return NULL; }
    s->slot = slot;
    s->addr = *a;
    link_random(l, r, 4);
    s->my_sid = (get32(r) & ~(uint32_t)(LINK_MAX_SESS - 1)) | (uint32_t)slot;
    s->created = s->last_rx = s->last_tx = l->now;
    s->tx_pn = 1;
    s->work_pos = -1;
    s->tag = SMNET_NO_ID;
    l->slots[slot] = s;
    s->active_pos = l->nactive;
    l->active[l->nactive++] = slot;
    hash_insert(l, s);
    l->st.sessions = l->nactive;
    present_front(l, s);
    return s;
}

link_t* link_new(const link_cfg_t* cfg, const uint8_t seed[32], link_out_fn out, link_in_fn in, void* ctx) {
    link_t* l = calloc(1, sizeof *l);
    if (!l) return NULL;
    if (cfg) l->cfg = *cfg; else link_cfg_default(&l->cfg);
    memcpy(l->seed, seed, 32);
    l->rng_pos = 64;
    l->out = out; l->in = in; l->ctx = ctx;
    for (int i = 0; i < NBUCKET; i++) l->bucket[i] = -1;
    uint8_t ks[32], zero[32] = { 0 };
    link_random(l, ks, 32);
    if (memcmp(l->cfg.identity_seed, zero, 32) != 0) memcpy(ks, l->cfg.identity_seed, 32);   /* identidad que dura */
    crypto_wipe(l->cfg.identity_seed, 32);
    crypto_eddsa_key_pair(l->id_sk, l->id_pk, ks);   /* borra ks */
    return l;
}

void link_free(link_t* l) {
    if (!l) return;
    while (l->nactive) sess_free(l, l->slots[l->active[0]]);
    crypto_wipe(l->seed, sizeof l->seed);
    free(l);
}

const link_stats_t* link_stats(const link_t* l) { return &l->st; }

/* --------------------------------------------------------------- claves */

static void derive(const link_t* l, sess_t* s, int initiator) {
    uint8_t shared[32], in[96], k[64];
    /* sin cifrado (bancos de escala): mismo formato y misma máquina de estados, sin X25519 */
    if (l->cfg.encrypt) {
        crypto_x25519(shared, s->eph_sk, s->peer_pk);
        /* punto de orden bajo: secreto nulo; se reemplaza por algo que no sirve al otro lado
           (la verificación del RESP/DATA falla y el handshake no prospera) */
        static const uint8_t zero[32] = { 0 };
        if (crypto_verify32(shared, zero) == 0) link_random((link_t*)l, shared, 32);
    } else memset(shared, 0, 32);
    memcpy(in, shared, 32);
    memcpy(in + 32, initiator ? s->eph_pk : s->peer_pk, 32);
    memcpy(in + 64, initiator ? s->peer_pk : s->eph_pk, 32);
    crypto_blake2b(k, 64, in, 96);
    memcpy(initiator ? s->k_tx : s->k_rx, k, 32);
    memcpy(initiator ? s->k_rx : s->k_tx, k + 32, 32);
    crypto_wipe(shared, 32); crypto_wipe(in, 96); crypto_wipe(k, 64);
    crypto_wipe(s->eph_sk, 32);
}

static void gen_eph(link_t* l, sess_t* s) {
    link_random(l, s->eph_sk, 32);
    if (l->cfg.encrypt) crypto_x25519_public_key(s->eph_pk, s->eph_sk);
    else link_random(l, s->eph_pk, 32);
}

static void make_nonce(uint8_t n[24], uint64_t pn) { memset(n, 0, 24); put64(n, pn); }

/* -------------------------------------------------------------- envío */

static void raw_out(link_t* l, sess_t* s, const uint8_t* d, int len) {
    s->last_tx = l->now;
    l->out(l->ctx, &s->addr, d, len);
}

static uint64_t wall_ts(const link_t* l) { return (uint64_t)(l->now + l->cfg.wall_offset_ms); }

static void sign_with_label(const link_t* l, uint8_t sig[64], const char* label, const uint8_t* a, int na, const uint8_t* b, int nb) {
    if (!l->cfg.encrypt) { memset(sig, 0, 64); return; }
    uint8_t m[256];
    int ll = (int)strlen(label);
    memcpy(m, label, (size_t)ll); memcpy(m + ll, a, (size_t)na); if (nb) memcpy(m + ll + na, b, (size_t)nb);
    crypto_eddsa_sign(sig, l->id_sk, m, (size_t)(ll + na + nb));
}
static int check_with_label(const link_t* l, const uint8_t sig[64], const uint8_t pk[32], const char* label, const uint8_t* a, int na, const uint8_t* b, int nb) {
    if (!l->cfg.encrypt) return 0;
    uint8_t m[256];
    int ll = (int)strlen(label);
    memcpy(m, label, (size_t)ll); memcpy(m + ll, a, (size_t)na); if (nb) memcpy(m + ll + na, b, (size_t)nb);
    return crypto_eddsa_check(sig, pk, m, (size_t)(ll + na + nb));
}

static void send_init(link_t* l, sess_t* s) {
    uint8_t p[INIT_SIZE] = { 0 };
    p[0] = PK_INIT; p[1] = LINK_VER;
    put32(p + 2, s->my_sid);
    memcpy(p + 6, s->eph_pk, 32);
    memcpy(p + 38, l->id_pk, 32);
    put64(p + 70, s->init_ts);
    sign_with_label(l, p + 78, "smnet-init", p, 78, NULL, 0);
    raw_out(l, s, p, INIT_SIZE);
    s->hs_next = l->now + l->cfg.hs_retry_ms;
    s->hs_tries++;
}

static void start_handshake(link_t* l, sess_t* s) {
    gen_eph(l, s);
    s->init_ts = wall_ts(l);
    s->state = S_INIT_SENT;
    s->is_responder = 0;
    s->hs_tries = 0;
    work_add(l, s);
    l->st.hs_started++;
    send_init(l, s);
}

/* Arma y manda un DATA con los frames ya escritos en l->plain[pos0..len). */
static void seal_send(link_t* l, sess_t* s, int plen, int ack_eliciting) {
    uint8_t* p = l->pkt;
    uint64_t pn = s->tx_pn++;
    p[0] = PK_DATA;
    put32(p + 1, s->their_sid);
    put64(p + 5, pn);
    if (l->cfg.encrypt) {
        uint8_t nonce[24];
        make_nonce(nonce, pn);
        crypto_aead_lock(p + DATA_HDR, p + DATA_HDR + plen, s->k_tx, nonce, p, DATA_HDR, l->plain, (size_t)plen);
    } else {
        memcpy(p + DATA_HDR, l->plain, (size_t)plen);
        memset(p + DATA_HDR + plen, 0, MAC);
    }
    (void)ack_eliciting;
    { sent_t* e = &s->sent[pn % SENT_RING]; e->pn = pn; e->t = l->now; }
    l->st.data_tx++;
    raw_out(l, s, p, DATA_HDR + plen + MAC);
}

static int write_ack(link_t* l, sess_t* s, uint8_t* p) {
    if (s->ack_need <= 0 || s->rx_max == 0) { p[0] = 0; return 1; }
    p[0] = F_ACK;
    put32(p + 1, (uint32_t)s->rx_max);
    put64(p + 5, s->rx_bits);
    s->ack_need--;
    (void)l;
    return 13;
}

static float rto_of(const sess_t* s) {
    float r = s->srtt > 0 ? s->srtt + 4 * s->rttvar : 300.f;
    if (r < 80) r = 80;
    if (r > 1000) r = 1000;
    return r;
}

/* Agrega retransmisiones vencidas que entren en el paquete. */
static int add_due_retx(link_t* l, sess_t* s, int pos, int limit, uint64_t pn_next, int force_all) {
    float rto = rto_of(s);
    for (int i = 0; i < s->relq_n; i++) {
        relmsg_t* m = &s->relq[i];
        if ((uint16_t)(m->rseq - s->relq[0].rseq) >= OOO_WIN) break;   /* ventana de envío */
        double wait = rto * (double)(1 << (m->tries > 1 ? 1 : 0));
        if (!force_all && m->tries && l->now - m->t_sent < wait) continue;
        if (pos + 5 + m->len > limit && !(pos <= ACK_ONLY && pos + 5 + m->len <= LINK_MAX_INNER)) continue;
        l->plain[pos] = FR_REL;
        put16(l->plain + pos + 1, m->len);
        put16(l->plain + pos + 3, m->rseq);
        memcpy(l->plain + pos + 5, m->data, m->len);
        pos += 5 + m->len;
        if (m->tries) l->st.rel_retx++;
        m->tries++;
        m->pn_last = pn_next;
        m->t_sent = l->now;
    }
    return pos;
}

static void flush_unrel(link_t* l, sess_t* s, const uint8_t* msg, int len) {
    int pos = write_ack(l, s, l->plain);
    l->plain[pos] = FR_UNREL;
    put16(l->plain + pos + 1, (uint16_t)len);
    memcpy(l->plain + pos + 3, msg, (size_t)len);
    pos += 3 + len;
    int before = pos;
    if (s->relq_n) pos = add_due_retx(l, s, pos, PACK_MAX, s->tx_pn, 0);
    seal_send(l, s, pos, pos != before);
}

/* Devuelve el mensaje encolado, o NULL si la cola está llena (se rechaza el nuevo: así nunca
 * queda un hueco en la numeración que trabe al receptor). */
static relmsg_t* queue_rel(link_t* l, sess_t* s, const uint8_t* msg, int len) {
    if (s->relq_n == RELQ_CAP) { l->st.rel_dropped++; return NULL; }
    relmsg_t* m = &s->relq[s->relq_n];
    m->data = malloc((size_t)len);
    if (!m->data) return NULL;
    memcpy(m->data, msg, (size_t)len);
    m->len = (uint16_t)len;
    m->rseq = s->tx_rseq++;
    m->tries = 0;
    m->pn_last = 0;
    m->t_sent = 0;
    s->relq_n++;
    l->st.rel_sent++;
    work_add(l, s);
    return m;
}

/* Manda todo lo pendiente de una sesión recién establecida (o retransmisiones vencidas). */
static void flush_rel(link_t* l, sess_t* s, int force_all) {
    for (int guard = 0; guard < 16; guard++) {
        int pos = write_ack(l, s, l->plain);
        int start = pos;
        pos = add_due_retx(l, s, pos, PACK_MAX, s->tx_pn, force_all);
        if (pos == start) {
            if (start > 1) seal_send(l, s, pos, 0);    /* quedó un ack */
            return;
        }
        seal_send(l, s, pos, 1);
        force_all = 0;
        /* lo que ya salió quedó con tries>0 y t_sent=now: no se repite en esta vuelta */
        int pending = 0;
        for (int i = 0; i < s->relq_n; i++) if (s->relq[i].tries == 0) pending = 1;
        if (!pending) return;
    }
}

static void on_established(link_t* l, sess_t* s) {
    s->state = S_EST;
    l->st.hs_ok++;
    /* primero lo confiable (la presentación va ahí y conviene que llegue antes que el estado) */
    if (s->relq_n) flush_rel(l, s, 1);
    for (int i = 0; i < s->pendq_n; i++) {
        flush_unrel(l, s, s->pendq[i].data, s->pendq[i].len);
        free(s->pendq[i].data);
    }
    s->pendq_n = 0;
}

void link_send(link_t* l, const smnet_addr_t* to, const uint8_t* msg, int len, int reliable, double now) {
    l->now = now;
    if (len <= 0 || len > LINK_MAX_INNER - 16) { l->st.malformed++; return; }
    sess_t* s = find_addr(l, to);
    if (!s) {
        s = sess_new(l, to);
        if (!s) return;
        start_handshake(l, s);
    }
    if (reliable) {
        relmsg_t* m = queue_rel(l, s, msg, len);
        if (m && s->state == S_EST && (uint16_t)(m->rseq - s->relq[0].rseq) < OOO_WIN) {
            /* manda solo lo nuevo (las retransmisiones vencidas van con él si entran) */
            int pos = write_ack(l, s, l->plain);
            l->plain[pos] = FR_REL;
            put16(l->plain + pos + 1, m->len);
            put16(l->plain + pos + 3, m->rseq);
            memcpy(l->plain + pos + 5, m->data, m->len);
            pos += 5 + m->len;
            m->tries = 1; m->pn_last = s->tx_pn; m->t_sent = now;
            pos = add_due_retx(l, s, pos, PACK_MAX, s->tx_pn, 0);
            seal_send(l, s, pos, 1);
        }
        return;
    }
    if (s->state == S_EST) { flush_unrel(l, s, msg, len); return; }
    /* sin sesión todavía: se guardan los últimos */
    if (s->pendq_n == PENDQ_CAP) { free(s->pendq[0].data); memmove(s->pendq, s->pendq + 1, sizeof(pend_t) * (PENDQ_CAP - 1)); s->pendq_n--; }
    pend_t* p = &s->pendq[s->pendq_n];
    p->data = malloc((size_t)len);
    if (!p->data) return;
    memcpy(p->data, msg, (size_t)len);
    p->len = (uint16_t)len;
    s->pendq_n++;
}

void link_punch(link_t* l, const smnet_addr_t* to, double now) {
    l->now = now;
    uint8_t p[5];
    p[0] = PK_PUNCH;
    link_random(l, p + 1, 4);
    l->st.punches_tx++;
    l->out(l->ctx, to, p, 5);
}

/* ------------------------------------------------------------ recepción */

static void handle_acks(link_t* l, sess_t* s, uint32_t largest32, uint64_t bits) {
    /* reconstruye el pn de 64 bits más cercano a lo que mandamos */
    uint64_t top = s->tx_pn - 1;
    uint64_t L = (top & ~(uint64_t)0xFFFFFFFFu) | largest32;
    if (L > top) L -= (uint64_t)1 << 32;
    if (L == 0 || L > top) return;
    if (L > s->acked_max) {
        s->acked_max = L;
        sent_t* e = &s->sent[L % SENT_RING];
        if (e->pn == L && e->t > 0) {
            float r = (float)(l->now - e->t);
            if (s->srtt <= 0) { s->srtt = r; s->rttvar = r / 2; }
            else { float d = r > s->srtt ? r - s->srtt : s->srtt - r; s->rttvar = 0.75f * s->rttvar + 0.25f * d; s->srtt = 0.875f * s->srtt + 0.125f * r; }
            e->t = 0;
        }
    }
    int w = 0;
    for (int i = 0; i < s->relq_n; i++) {
        relmsg_t* m = &s->relq[i];
        int acked = 0;
        if (m->tries && m->pn_last) {
            if (m->pn_last == L) acked = 1;
            else if (m->pn_last < L && L - m->pn_last <= 64) acked = (int)((bits >> (L - m->pn_last - 1)) & 1);
        }
        if (acked) { free(m->data); continue; }
        /* retransmisión rápida: ya se confirmaron 3 paquetes posteriores y este no */
        if (m->tries && m->pn_last && m->pn_last + 3 <= L) m->t_sent = -1e18;
        s->relq[w++] = *m;
    }
    s->relq_n = w;
}

static void deliver_rel(link_t* l, sess_t* s, uint16_t rseq, const uint8_t* d, int len) {
    int16_t diff = (int16_t)(rseq - s->rx_rseq);
    if (diff < 0) return;                    /* duplicado: ya entregado */
    if (diff >= OOO_WIN) return;             /* demasiado adelante: el otro lo repetirá */
    if (diff > 0) {
        ooo_t* o = &s->ooo[rseq % OOO_WIN];
        if (o->have) return;
        o->data = malloc((size_t)len);
        if (!o->data) return;
        memcpy(o->data, d, (size_t)len);
        o->len = (uint16_t)len;
        o->have = 1;
        return;
    }
    s->rx_rseq++;
    l->st.rel_delivered++;
    smnet_addr_t from = s->addr;
    l->in(l->ctx, &from, d, len);
    for (;;) {
        ooo_t* o = &s->ooo[s->rx_rseq % OOO_WIN];
        if (!o->have) break;
        uint8_t* data = o->data; uint16_t olen = o->len;
        o->have = 0; o->data = NULL;
        s->rx_rseq++;
        l->st.rel_delivered++;
        l->in(l->ctx, &from, data, olen);
        free(data);
    }
}

/* Vuelve a numerar desde cero lo pendiente: el otro lado perdió el estado de la sesión. */
static void reset_numbering(link_t* l, sess_t* s) {
    s->tx_pn = 1; s->rx_max = s->rx_bits = 0; s->acked_max = 0;
    s->rx_rseq = 0; s->srtt = s->rttvar = 0; s->ack_need = 0;
    memset(s->sent, 0, sizeof s->sent);
    for (int i = 0; i < OOO_WIN; i++) { free(s->ooo[i].data); s->ooo[i].data = NULL; s->ooo[i].have = 0; }
    present_front(l, s);   /* sesión nueva del otro lado: sin nuestra etiqueta */
    s->tx_rseq = 0;
    for (int i = 0; i < s->relq_n; i++) { s->relq[i].rseq = s->tx_rseq++; s->relq[i].tries = 0; s->relq[i].pn_last = 0; }
    if (s->relq_n) work_add(l, s);
}

/* Marca de tiempo de INIT por identidad: tiene que crecer (un INIT capturado no se puede repetir). */
static int ts_fresh(link_t* l, const uint8_t idpk[32], uint64_t ts, int commit) {
    uint64_t key = get64(idpk) | 1;
    int k = (int)(get32(idpk + 8) & (TS_SLOTS - 1));
    if (l->seen_ts[k].key == key && ts <= l->seen_ts[k].ts) return 0;
    if (commit) { l->seen_ts[k].key = key; l->seen_ts[k].ts = ts; }
    return 1;
}

static void on_init(link_t* l, const smnet_addr_t* from, const uint8_t* d, int len) {
    if (len != INIT_SIZE || d[1] != LINK_VER) { l->st.malformed++; return; }
    uint32_t sid_i = get32(d + 2);
    const uint8_t* pk = d + 6;
    const uint8_t* idpk = d + 38;
    uint64_t ts = get64(d + 70);
    sess_t* s = find_addr(l, from);
    /* repetido = misma clave efímera (no alcanza con el sid: un re-handshake lo puede repetir) */
    int same = s && s->their_sid == sid_i && memcmp(s->peer_pk, pk, 32) == 0;
    if (s && s->state == S_EST && s->is_responder && same) {
        raw_out(l, s, s->resp, RESP_SIZE);   /* el RESP se perdió */
        return;
    }
    if (s && s->state == S_EST && !s->is_responder && same) return;   /* INIT viejo de una apertura simultánea */
    /* identidad: firma del iniciador sobre todo el INIT y marca de tiempo creciente */
    if (check_with_label(l, d + 78, idpk, "smnet-init", d, 78, NULL, 0) != 0) { l->st.bad_sig++; return; }
    if (l->cfg.encrypt && !ts_fresh(l, idpk, ts, 0)) { l->st.replays++; return; }
    if (s && s->state == S_INIT_SENT) {
        int c = memcmp(s->eph_pk, pk, 32);
        if (c == 0) c = s->my_sid > sid_i ? 1 : -1;      /* empate exacto (semillas iguales): por sid */
        if (c > 0) return;                               /* el otro hace de respondedor */
        l->st.hs_simultaneous++;
    } else if (s && s->state == S_EST) {
        /* el otro reinició: sesión nueva, se renumera lo pendiente */
        l->st.hs_restarts++;
        reset_numbering(l, s);
        gen_eph(l, s);
    } else {
        s = sess_new(l, from);
        if (!s) return;
        gen_eph(l, s);
    }
    if (l->cfg.encrypt) ts_fresh(l, idpk, ts, 1);
    if (memcmp(s->peer_id, idpk, 32) != 0) { s->tag = SMNET_NO_ID; s->presented = 0; memcpy(s->peer_id, idpk, 32); }
    s->is_responder = 1;
    s->their_sid = sid_i;
    memcpy(s->peer_pk, pk, 32);
    derive(l, s, 0);
    uint8_t* r = s->resp;
    r[0] = PK_RESP;
    put32(r + 1, s->my_sid);
    put32(r + 5, sid_i);
    memcpy(r + 9, s->eph_pk, 32);
    memcpy(r + 41, l->id_pk, 32);
    /* la firma del respondedor cubre también la clave efímera y la identidad del iniciador */
    uint8_t tail[64];
    memcpy(tail, pk, 32); memcpy(tail + 32, idpk, 32);
    sign_with_label(l, r + 73, "smnet-resp", r, 73, tail, 64);
    uint8_t nonce[24], dummy[1] = { 0 };
    memset(nonce, 0xFF, 24);
    crypto_aead_lock(dummy, r + 137, s->k_tx, nonce, r, 137, dummy, 0);
    s->last_rx = l->now;
    l->st.hs_responded++;
    raw_out(l, s, r, RESP_SIZE);
    on_established(l, s);
}

static void on_resp(link_t* l, const smnet_addr_t* from, const uint8_t* d, int len) {
    if (len != RESP_SIZE) { l->st.malformed++; return; }
    sess_t* s = find_sid(l, get32(d + 5));
    if (!s || s->state != S_INIT_SENT) return;
    const uint8_t* idpk = d + 41;
    uint8_t tail[64];
    memcpy(tail, s->eph_pk, 32); memcpy(tail + 32, l->id_pk, 32);
    if (check_with_label(l, d + 73, idpk, "smnet-resp", d, 73, tail, 64) != 0) { l->st.bad_sig++; return; }
    memcpy(s->peer_pk, d + 9, 32);
    uint8_t ksk[32];
    memcpy(ksk, s->eph_sk, 32);
    derive(l, s, 1);
    uint8_t nonce[24], dummy[1] = { 0 };
    memset(nonce, 0xFF, 24);
    if (crypto_aead_unlock(dummy, d + 137, s->k_rx, nonce, d, 137, dummy, 0) != 0) {
        /* RESP falso o corrupto: se vuelve a la espera con la misma clave efímera */
        memcpy(s->eph_sk, ksk, 32);
        crypto_wipe(ksk, 32);
        l->st.bad_mac++;
        return;
    }
    crypto_wipe(ksk, 32);
    if (memcmp(s->peer_id, idpk, 32) != 0) { s->tag = SMNET_NO_ID; s->presented = 0; memcpy(s->peer_id, idpk, 32); }
    s->their_sid = get32(d + 1);
    s->last_rx = l->now;
    if (!smnet_addr_eq(from, &s->addr)) { hash_remove(l, s); s->addr = *from; hash_insert(l, s); l->st.roams++; }
    on_established(l, s);
}

static void on_data(link_t* l, const smnet_addr_t* from, const uint8_t* d, int len) {
    if (len < DATA_HDR + MAC + 1) { l->st.malformed++; return; }
    sess_t* s = find_sid(l, get32(d + 1));
    if (s && s->state == S_INIT_SENT) { l->st.no_session++; return; }   /* llegó antes que el RESP: sin RESET */
    if (!s || s->state != S_EST || s->closing) {
        l->st.no_session++;
        /* le avisamos que no tenemos esa sesión (con límite de tasa: no amplifica, 5 B por ≥ 30 B) */
        if (l->now - l->reset_window > 1000) { l->reset_window = l->now; l->resets_in_window = 0; }
        if (l->resets_in_window++ < 64) {
            uint8_t p[5];
            p[0] = PK_RESET;
            memcpy(p + 1, d + 1, 4);
            l->out(l->ctx, from, p, 5);
        }
        return;
    }
    uint64_t pn = get64(d + 5);
    if (pn == 0) { l->st.malformed++; return; }
    if (pn <= s->rx_max) {
        uint64_t k = s->rx_max - pn;
        if (k == 0 || k > 64 || ((s->rx_bits >> (k - 1)) & 1)) { l->st.replays++; return; }
    }
    int plen = len - DATA_HDR - MAC;
    if (plen > LINK_MAX_INNER + 64) { l->st.malformed++; return; }   /* más grande que cualquier cosa que mandamos */
    if (l->cfg.encrypt) {
        uint8_t nonce[24];
        make_nonce(nonce, pn);
        if (crypto_aead_unlock(l->plain, d + DATA_HDR + plen, s->k_rx, nonce, d, DATA_HDR, d + DATA_HDR, (size_t)plen) != 0) {
            l->st.bad_mac++;
            return;
        }
    } else memcpy(l->plain, d + DATA_HDR, (size_t)plen);
    /* auténtico: ventana */
    int newest = pn > s->rx_max;
    if (pn > s->rx_max) {
        uint64_t sh = pn - s->rx_max;
        s->rx_bits = sh >= 64 ? 0 : (s->rx_bits << sh);
        if (sh <= 64 && s->rx_max) s->rx_bits |= (uint64_t)1 << (sh - 1);
        s->rx_max = pn;
    } else s->rx_bits |= (uint64_t)1 << (s->rx_max - pn - 1);
    s->last_rx = l->now;
    l->st.data_rx++;
    s->authed = 1;
    /* cambio de dirección (NAT que remapeó): solo con el paquete más nuevo, no con uno demorado */
    if (newest && !smnet_addr_eq(from, &s->addr)) { hash_remove(l, s); s->addr = *from; hash_insert(l, s); l->st.roams++; }

    /* se copia: los callbacks pueden mandar y reusar l->plain */
    uint8_t buf[LINK_MAX_INNER + 128];
    memcpy(buf, l->plain, (size_t)plen);
    l->busy = s;
    int pos = 0;
    uint8_t flags = buf[pos++];
    if (flags & F_ACK) {
        if (pos + 12 > plen) { l->st.malformed++; l->busy = NULL; return; }
        handle_acks(l, s, get32(buf + pos), get64(buf + pos + 4));
        pos += 12;
    }
    int has_rel = 0;
    while (pos + 3 <= plen) {
        uint8_t kind = buf[pos];
        int flen = get16(buf + pos + 1);
        if (kind == FR_UNREL) {
            if (pos + 3 + flen > plen) { l->st.malformed++; break; }
            smnet_addr_t a = s->addr;
            l->in(l->ctx, &a, buf + pos + 3, flen);
            pos += 3 + flen;
        } else if (kind == FR_REL) {
            if (pos + 5 + flen > plen) { l->st.malformed++; break; }
            has_rel = 1;
            /* la sesión puede liberarse en un callback solo vía link_close, que se difiere (ver abajo) */
            deliver_rel(l, s, get16(buf + pos + 3), buf + pos + 5, flen);
            pos += 5 + flen;
        } else { l->st.malformed++; break; }
    }
    l->busy = NULL;
    if (has_rel) {
        work_add(l, s);
        s->ack_need = 2;
        if (s->ack_due <= l->now) s->ack_due = l->now + l->cfg.ack_delay_ms;
    }
}

void link_receive(link_t* l, const smnet_addr_t* from, const uint8_t* d, int len, double now) {
    l->now = now;
    if (len < 1 || len > LINK_MAX_INNER + 128) { l->st.malformed++; return; }
    switch (d[0]) {
    case PK_DATA:  on_data(l, from, d, len); break;
    case PK_INIT:  on_init(l, from, d, len); break;
    case PK_RESP:  on_resp(l, from, d, len); break;
    case PK_PUNCH: l->st.punches_rx++; break;
    case PK_RESET: {
        if (len != 5) { l->st.malformed++; break; }
        sess_t* s = find_addr(l, from);
        /* solo si nombra la sesión que usamos hacia ese destino (un tercero a ciegas no la conoce) */
        if (s && s->state == S_EST && s->their_sid == get32(d + 1)) {
            l->st.hs_restarts++;
            /* sid nuevo: que el otro no confunda el INIT con uno repetido de la sesión vieja */
            uint8_t rnd[4];
            link_random(l, rnd, 4);
            s->my_sid = (get32(rnd) & ~(uint32_t)(LINK_MAX_SESS - 1)) | (uint32_t)s->slot;
            reset_numbering(l, s);
            start_handshake(l, s);
        }
        break;
    }
    default:       l->st.malformed++;
    }
}

/* ------------------------------------------------------------ mantenimiento */

static void session_work(link_t* l, sess_t* s, double now) {
    /* confiables vencidos: no se abandona nada mientras la sesión viva (si el otro murió, expira) */
    if (s->relq_n) {
        float rto = rto_of(s);
        int due = 0;
        for (int k = 0; k < s->relq_n && !due; k++) {
            relmsg_t* m = &s->relq[k];
            if ((uint16_t)(m->rseq - s->relq[0].rseq) >= OOO_WIN) break;
            double wait = rto * (double)(1 << (m->tries > 1 ? 1 : 0));
            if (!m->tries || now - m->t_sent >= wait) due = 1;
        }
        if (due) flush_rel(l, s, 0);
    }
    if (s->ack_need > 0 && now >= s->ack_due) {
        int pos = write_ack(l, s, l->plain);
        s->ack_need = 0;
        l->st.pure_acks++;
        seal_send(l, s, pos, 0);
    }
}

void link_update(link_t* l, double now) {
    l->now = now;
    /* expiración: barrido completo una vez por segundo */
    if (now >= l->next_scan) {
        l->next_scan = now + 1000;
        for (int i = 0; i < l->nactive; i++) {
            sess_t* s = l->slots[l->active[i]];
            /* vence si está quieta en los dos sentidos, o si hace 2 minutos que no llega nada
               (flujos de un solo sentido, como METRICS al coordinador, no la hacen rotar) */
            double quiet = now - (s->last_rx > s->last_tx ? s->last_rx : s->last_tx);
            if (s->state == S_EST && (quiet > l->cfg.idle_expire_ms || now - s->last_rx > 4 * l->cfg.idle_expire_ms)) { l->st.expired++; sess_free(l, s); i--; }
        }
    }
    /* solo las sesiones con algo pendiente */
    for (int i = 0; i < l->nwork; i++) {
        sess_t* s = l->slots[l->work[i]];
        if (s->closing) { sess_free(l, s); i--; continue; }
        if (s->state == S_INIT_SENT) {
            if (now >= s->hs_next) {
                if (s->hs_tries >= l->cfg.hs_tries) { l->st.hs_failed++; sess_free(l, s); i--; continue; }
                send_init(l, s);
            }
            continue;
        }
        session_work(l, s, now);
        if (s->relq_n == 0 && s->ack_need <= 0) { work_del(l, s); i--; }
    }
}

int link_established(const link_t* l, const smnet_addr_t* to) {
    const sess_t* s = find_addr(l, to);
    return s && s->state == S_EST;
}

float link_srtt(const link_t* l, const smnet_addr_t* to) {
    const sess_t* s = find_addr(l, to);
    return s ? s->srtt : 0.f;
}

void link_close(link_t* l, const smnet_addr_t* to) {
    sess_t* s = find_addr(l, to);
    if (s) { s->closing = 1; work_add(l, s); }
}

int link_pending(const link_t* l, const smnet_addr_t* to) {
    const sess_t* s = find_addr(l, to);
    return s ? s->relq_n : -1;
}

const uint8_t* link_identity(const link_t* l) { return l->id_pk; }
void link_sign(const link_t* l, uint8_t sig[64], const uint8_t* msg, int len) {
    if (!l->cfg.encrypt) { memset(sig, 0, 64); return; }
    crypto_eddsa_sign(sig, l->id_sk, msg, (size_t)len);
}
const uint8_t* link_peer_identity(const link_t* l, const smnet_addr_t* a) {
    const sess_t* s = find_addr(l, a);
    return s && s->state == S_EST ? s->peer_id : NULL;
}
uint16_t link_tag(const link_t* l, const smnet_addr_t* a) {
    const sess_t* s = find_addr(l, a);
    return s && s->state == S_EST ? s->tag : SMNET_NO_ID;
}
void link_set_tag(link_t* l, const smnet_addr_t* a, uint16_t tag) {
    sess_t* s = find_addr(l, a);
    if (s) s->tag = tag;
}
int link_presented(const link_t* l, const smnet_addr_t* a) {
    const sess_t* s = find_addr(l, a);
    return s && s->presented;
}
void link_set_presented(link_t* l, const smnet_addr_t* a) {
    sess_t* s = find_addr(l, a);
    if (s) s->presented = 1;
}
void link_set_presentation(link_t* l, const uint8_t* msg, int len) {
    if (len <= 0 || len > (int)sizeof l->pres) return;
    int changed = l->pres_len != len || memcmp(l->pres, msg, (size_t)len) != 0;
    memcpy(l->pres, msg, (size_t)len);
    l->pres_len = len;
    if (!changed) return;
    /* sesiones ya abiertas: la nueva presentación va con lo siguiente que se mande */
    for (int i = 0; i < l->nactive; i++) {
        sess_t* s = l->slots[l->active[i]];
        if (queue_rel(l, s, msg, len)) { s->presented = 1; work_add(l, s); }
    }
}
