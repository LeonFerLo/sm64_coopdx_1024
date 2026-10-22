/* Atajo directo estable (PROTOCOL.md §9.4, v1.3).
 *
 * Un peer abre un enlace directo con un vecino cercano y estable para que su estado llegue sin
 * pasar por los SN. El estado se sigue mandando al SN (el enlace solo reemplaza la entrega
 * SN -> vecino, y el SN se lo baja a 2 Hz a quien lo recibe directo).
 *
 *   señalización : LINK_OFFER / LINK_ANSWER por los SN (etapa 0 peer->SN, 1 SN->SN, 2 SN->peer);
 *                  cada SN agrega la dirección del suyo tal como la ve (sirve para perforar NAT)
 *   apertura     : los dos perforan y mandan STATE_DIRECT; la capa de enlace resuelve el
 *                  handshake (apertura simultánea incluida)
 *   ganancia     : se MIDE: para el mismo seq, llegada por el SN − llegada directa (EWMA)
 *   política     : 32 más cercanos hace ≥ 10 s, velocidad relativa ≤ 300 u/s, ≤ 8 enlaces y
 *                  ≤ 30% del upload, token bucket 4/min (ráfaga 8), prueba de 5 s con ganancia
 *                  ≥ 10 ms, se conserva con ≥ 3 ms (vida mínima 30 s), backoff 30 s × 2^fallas
 *                  hasta 10 min. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "node_internal.h"

#define NEAR_K         32
#define MIN_NEIGHBOR   10000.0
#define MAX_REL_SPEED  300.f
#define OPEN_GAIN      10.f
#define KEEP_GAIN      3.f
#define PROBATION      5000.0
#define MIN_LIFE       30000.0
#define SETUP_TIMEOUT  5000.0
#define LOST_AFTER     1500.0
#define TOKENS_PER_MIN 4.0
#define TOKENS_BURST   8.0
#define BACKOFF_BASE   30000.0
#define BACKOFF_MAX    600000.0
#define LINK_KBPS      20.f            /* 30 Hz × ~80 B en el cable */

int direct_init(smnet_node_t* n) {
    direct_t* d = &n->peer.dir;
    memset(d, 0, sizeof *d);
    d->near_since = calloc(SMNET_MAX_PLAYERS, sizeof(double));
    d->retry_after = calloc(SMNET_MAX_PLAYERS, sizeof(double));
    d->fails = calloc(SMNET_MAX_PLAYERS, 1);
    d->rank = malloc(SMNET_MAX_PLAYERS);
    d->tmp = malloc(sizeof(struct dd) * SMNET_MAX_PLAYERS);
    if (!d->near_since || !d->retry_after || !d->fails || !d->rank || !d->tmp) return -1;
    memset(d->rank, 255, SMNET_MAX_PLAYERS);
    d->tokens = TOKENS_BURST;
    return 0;
}

void direct_free(smnet_node_t* n) {
    direct_t* d = &n->peer.dir;
    free(d->near_since); free(d->retry_after); free(d->fails); free(d->rank); free(d->tmp);
    memset(d, 0, sizeof *d);
}

static dlink_t* find_link(direct_t* d, uint16_t id) {
    for (int i = 0; i < d->nl; i++) if (d->l[i].id == id) return &d->l[i];
    return NULL;
}

static void backoff(direct_t* d, uint16_t id, double now) {
    if (id >= SMNET_MAX_PLAYERS) return;
    if (d->fails[id] < 10) d->fails[id]++;
    double b = BACKOFF_BASE * (double)(1u << (d->fails[id] - 1));
    if (b > BACKOFF_MAX) b = BACKOFF_MAX;
    d->retry_after[id] = now + b;
}

static void drop_link(smnet_node_t* n, dlink_t* l, int notify, double now) {
    direct_t* d = &n->peer.dir;
    if (notify && l->state != DL_OFFERED) {
        uint8_t buf[16];
        wbuf_t w = { buf, 0, sizeof buf, 0 };
        node_hdr(n, &w, MSG_LINK_CLOSE, 0, 0);
        w_u32(&w, l->token);
        node_send_addr(n, &l->addr, buf, w.len);
    }
    if (l->state == DL_UP) { d->life_sum += now - l->t_up; d->life_n++; }
    int i = (int)(l - d->l);
    d->l[i] = d->l[--d->nl];
    d->next_set = 0;   /* avisar ya al SN que vuelva a mandarlo a tasa normal */
}

/* NAT compatibles (§9.4.1): cono-cono se intenta; simétrico solo con abierto. */
static int nat_compat(uint8_t a, uint8_t b) {
    if (a == SMNET_NAT_SYMMETRIC) return b == SMNET_NAT_OPEN;
    if (b == SMNET_NAT_SYMMETRIC) return a == SMNET_NAT_OPEN;
    return 1;
}

static int budget_ok(const smnet_node_t* n, int links) {
    int cap = n->cfg.direct_max < DIRECT_MAX ? n->cfg.direct_max : DIRECT_MAX;
    return links < cap && (float)(links + 1) * LINK_KBPS <= n->cfg.direct_upload_frac * (float)n->cfg.upload_kbps;
}

static void vel_of(const smnet_state_t* st, float* vx, float* vz) {
    float a = (float)st->yaw * 3.14159265f / 32768.f;
    float sp = st->fwd_vel * 30.f;   /* unidades por cuadro a 30 fps -> por segundo */
    *vx = sinf(a) * sp;
    *vz = cosf(a) * sp;
}

/* ------------------------------------------------------------- medición */

void direct_note(smnet_node_t* n, uint16_t id, uint8_t seq, double now, int via_sn) {
    direct_t* d = &n->peer.dir;
    if (!d->nl) return;
    dlink_t* l = find_link(d, id);
    if (!l || l->state != DL_UP) return;
    int k = seq % GAIN_RING;
    float sample = NAN;
    if (l->g_seq[k] != seq || (l->g_td[k] == 0 && l->g_ts[k] == 0)) {
        l->g_seq[k] = seq; l->g_td[k] = 0; l->g_ts[k] = 0;
    }
    if (via_sn) {
        if (l->g_ts[k] == 0) { l->g_ts[k] = now; if (l->g_td[k] > 0) sample = (float)(now - l->g_td[k]); }
    } else {
        if (l->g_td[k] == 0) { l->g_td[k] = now; if (l->g_ts[k] > 0) sample = (float)(l->g_ts[k] - now); }
    }
    if (!isnan(sample)) {
        if (sample > 500) sample = 500;
        if (sample < -500) sample = -500;
        l->gain_ms = l->gain_n ? 0.8f * l->gain_ms + 0.2f * sample : sample;
        l->gain_n++;
    }
}

/* ------------------------------------------------------------- mensajes */

static void send_offer_or_answer(smnet_node_t* n, uint8_t type, uint16_t other, uint32_t token, int accept) {
    peer_t* p = &n->peer;
    if (p->primary.id == SMNET_NO_ID) return;
    uint8_t buf[32];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    node_hdr(n, &w, type, 0, 0);
    w_u8(&w, 0);              /* etapa 0: al propio SN */
    w_u16(&w, n->id);
    w_u16(&w, other);
    w_u32(&w, token);
    w_u8(&w, n->cfg.nat);
    if (type == MSG_LINK_ANSWER) w_u8(&w, (uint8_t)accept);   /* 0 rechazo, 1 acepto, 2 NAT incompatible */
    node_send_id(n, p->primary.id, &p->primary.addr, buf, w.len);
}

void direct_send_state(smnet_node_t* n, const uint8_t* msg, int len, const uint8_t* blobmsg, int bloblen) {
    direct_t* d = &n->peer.dir;
    float r2 = n->cfg.sn.full_radius * n->cfg.sn.full_radius;
    for (int i = 0; i < d->nl; i++) {
        dlink_t* l = &d->l[i];
        if (l->state == DL_OFFERED) continue;
        /* el estado rico solo a los que están cerca (como haría el SN con sus 4 más cercanos) */
        const remote_t* rm = &n->peer.remotes[l->id];
        int rich = 0;
        if (blobmsg && rm->valid) {
            float dx = rm->st.x - n->peer.local.x, dz = rm->st.z - n->peer.local.z;
            rich = dx * dx + dz * dz <= r2;
        }
        if (rich) node_send_addr(n, &l->addr, blobmsg, bloblen);
        else node_send_addr(n, &l->addr, msg, len);
    }
}

int direct_receive(smnet_node_t* n, const smnet_addr_t* from, const smnet_hdr_t* h, rbuf_t* r, double now) {
    peer_t* p = &n->peer;
    direct_t* d = &p->dir;
    switch (h->type) {
    case MSG_STATE_DIRECT: {
        dlink_t* l = find_link(d, h->sender);
        if (!l || l->state == DL_OFFERED) return 0;
        /* abierto: solo desde la dirección del enlace (mientras se abre se acepta la primera,
           que puede diferir de la anunciada si el otro tiene NAT simétrico) */
        if (l->state == DL_UP && !smnet_addr_eq(from, &l->addr)) return 0;
        uint16_t area = r_u16(r);
        smnet_state_t st;
        state_read(r, &st);
        if (r->err) return 0;
        (void)area;
        if (l->state == DL_OPENING) { l->state = DL_UP; l->t_up = now; if (l->mine) d->opens++; d->fails[l->id] = 0; }
        l->addr = *from;
        l->last_rx = now;
        uint8_t seq = (uint8_t)h->seq;
        direct_note(n, h->sender, seq, now, 0);
        peer_accept_remote(n, h->sender, seq, &st, now);
        if (h->flags & UP_HAS_BLOB) {
            uint16_t bl = r_u16(r);
            uint8_t blob[SMNET_MAX_BLOB];
            if (bl <= SMNET_MAX_BLOB) {
                r_bytes(r, blob, bl);
                if (!r->err) { n->stats.blob_updates++; if (n->on_blob) n->on_blob(n->user, h->sender, seq, blob, bl, now); }
            }
        }
        return 1;
    }
    case MSG_LINK_CLOSE: {
        uint32_t token = r_u32(r);
        if (r->err) return 0;
        dlink_t* cl = find_link(d, h->sender);
        if (cl && cl->state == DL_UP && !smnet_addr_eq(from, &cl->addr)) return 0;
        for (int i = 0; i < d->nl; i++)
            if (d->l[i].id == h->sender && d->l[i].token == token) { drop_link(n, &d->l[i], 0, now); break; }
        return 1;
    }
    case MSG_LINK_OFFER: {
        if (!smnet_addr_eq(from, &p->primary.addr) && !(p->next.id != SMNET_NO_ID && smnet_addr_eq(from, &p->next.addr))) return 0;
        r_u8(r);                    /* etapa */
        uint16_t src = r_u16(r), dst = r_u16(r);
        uint32_t token = r_u32(r);
        uint8_t nat = r_u8(r);
        smnet_addr_t a;
        addr_read(r, &a);
        if (r->err || dst != n->id || src >= SMNET_MAX_PLAYERS || !n->cfg.direct) return 0;
        dlink_t* l = find_link(d, src);
        if (l && l->state == DL_OFFERED) {
            /* ofertas cruzadas: gana la del id menor */
            if (n->id < src) return 1;
            l->token = token;
            l->mine = 0;
            d->attempts--;      /* nuestra oferta quedó absorbida por la del otro */
            d->tokens += 1.0;
        } else if (l) return 1;    /* ya hay enlace */
        int nat_ok = nat_compat(n->cfg.nat, nat);
        int ok = p->status == SMNET_PEER_CONNECTED && nat_ok && now >= d->retry_after[src] && (l || budget_ok(n, d->nl));
        if (!ok) { d->rejected++; send_offer_or_answer(n, MSG_LINK_ANSWER, src, token, nat_ok ? 0 : 2); if (l) drop_link(n, l, 0, now); return 1; }
        if (!l) { if (d->nl >= DIRECT_MAX) return 1; l = &d->l[d->nl++]; memset(l, 0, sizeof *l); }
        l->id = src; l->state = DL_OPENING; l->nat = nat; l->addr = a; l->token = token; l->t_start = now;
        d->accepted++;
        node_punch(n, &a);
        send_offer_or_answer(n, MSG_LINK_ANSWER, src, token, 1);
        return 1;
    }
    case MSG_LINK_ANSWER: {
        if (!smnet_addr_eq(from, &p->primary.addr) && !(p->next.id != SMNET_NO_ID && smnet_addr_eq(from, &p->next.addr))) return 0;
        r_u8(r);
        uint16_t src = r_u16(r), dst = r_u16(r);
        uint32_t token = r_u32(r);
        uint8_t nat = r_u8(r);
        uint8_t accept = r_u8(r);
        smnet_addr_t a;
        addr_read(r, &a);
        if (r->err || dst != n->id) return 0;
        dlink_t* l = find_link(d, src);
        if (!l || l->state != DL_OFFERED || l->token != token) return 1;
        if (accept != 1) {
            d->failed++;
            if (accept == 2) {
                /* NAT incompatible: no va a cambiar; se devuelve el token y no se reintenta por 10 min */
                d->f_nat++;
                d->tokens += 1.0;
                d->retry_after[src] = now + BACKOFF_MAX;
            } else { d->f_rejected++; backoff(d, src, now); }
            drop_link(n, l, 0, now);
            return 1;
        }
        l->state = DL_OPENING; l->nat = nat; l->addr = a; l->t_start = now;
        node_punch(n, &a);
        return 1;
    }
    }
    return 0;
}

/* -------------------------------------------------------------- política */

static int cmp_dd(const void* a, const void* b) { float x = ((const struct dd*)a)->d, y = ((const struct dd*)b)->d; return (x > y) - (x < y); }

static void send_set(smnet_node_t* n) {
    peer_t* p = &n->peer;
    direct_t* d = &p->dir;
    if (p->primary.id == SMNET_NO_ID) return;
    uint8_t buf[SMNET_HDR_SIZE + 1 + 2 * DIRECT_MAX];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    node_hdr(n, &w, MSG_DIRECT_SET, 0, 0);
    int cpos = w.len, cnt = 0;
    w_u8(&w, 0);
    for (int i = 0; i < d->nl; i++) {
        dlink_t* l = &d->l[i];
        if (l->state != DL_UP || n->now - l->last_rx > 500) continue;
        if (l->gain_n >= 3 && l->gain_ms < 0) continue;     /* el SN es más rápido: que siga mandando */
        w_u16(&w, l->id);
        cnt++;
    }
    buf[cpos] = (uint8_t)cnt;
    node_send_id(n, p->primary.id, &p->primary.addr, buf, w.len);
}

void direct_update(smnet_node_t* n, double now) {
    peer_t* p = &n->peer;
    direct_t* d = &p->dir;
    if (!n->cfg.direct || p->status != SMNET_PEER_CONNECTED || n->sn) {
        /* un SN no abre atajos (ya está en el camino); si lo promueven, se cierran */
        while (d->nl) drop_link(n, &d->l[0], 1, now);
        return;
    }
    /* recarga de tokens */
    if (d->last_refill == 0) d->last_refill = now;
    d->tokens += (now - d->last_refill) / 60000.0 * TOKENS_PER_MIN;
    if (d->tokens > TOKENS_BURST) d->tokens = TOKENS_BURST;
    d->last_refill = now;

    /* vencimientos (cada vez) */
    for (int i = 0; i < d->nl; i++) {
        dlink_t* l = &d->l[i];
        int drop = 0, notify = 1;
        if (l->state != DL_UP && now - l->t_start > SETUP_TIMEOUT) {
            if (l->mine) { d->failed++; if (l->state == DL_OFFERED) d->f_noanswer++; else d->f_timeout++; }
            backoff(d, l->id, now); drop = 1; notify = l->state == DL_OPENING;
        }
        else if (l->state == DL_UP && now - l->last_rx > LOST_AFTER) { d->lost++; backoff(d, l->id, now); drop = 1; }
        if (drop) { drop_link(n, l, notify, now); i--; }
    }
    if (now >= d->next_set) { send_set(n); d->next_set = now + 1000; }
    if (now < d->next_eval) return;
    d->next_eval = now + n->cfg.direct_eval_ms;

    /* ranking por cercanía de los remotos vigentes */
    int cnt = 0;
    for (int e = 0; e < SMNET_MAX_PLAYERS; e++) {
        const remote_t* rm = &p->remotes[e];
        d->rank[e] = 255;
        if (e == n->id || !rm->valid || now - rm->last_rx_ms > 1000) continue;
        float dx = rm->st.x - p->local.x, dy = rm->st.y - p->local.y, dz = rm->st.z - p->local.z;
        d->tmp[cnt].d = dx * dx + dy * dy + dz * dz;
        d->tmp[cnt].id = (uint16_t)e;
        cnt++;
    }
    qsort(d->tmp, (size_t)cnt, sizeof *d->tmp, cmp_dd);
    for (int k = 0; k < cnt && k < 254; k++) d->rank[d->tmp[k].id] = (uint8_t)k;
    for (int e = 0; e < SMNET_MAX_PLAYERS; e++) {
        if (d->rank[e] < NEAR_K) { if (d->near_since[e] == 0) d->near_since[e] = now; }
        else d->near_since[e] = 0;
    }

    /* conservar / cerrar */
    for (int i = 0; i < d->nl; i++) {
        dlink_t* l = &d->l[i];
        if (l->state != DL_UP) continue;
        double life = now - l->t_up;
        int drop = 0;
        if (d->rank[l->id] == 255) drop = 1;                                   /* se fue del área */
        else if (life >= PROBATION && life < MIN_LIFE && l->gain_n >= 5 && l->gain_ms < OPEN_GAIN) drop = 1;
        else if (life >= MIN_LIFE && l->gain_n >= 5 && l->gain_ms < KEEP_GAIN) drop = 1;
        if (drop) {
            if (d->rank[l->id] != 255) { d->closed_nogain++; backoff(d, l->id, now); }
            drop_link(n, l, 1, now); i--;
        }
    }

    /* abrir */
    float mvx, mvz;
    vel_of(&p->local, &mvx, &mvz);
    for (int k = 0; k < cnt && k < NEAR_K && d->tokens >= 1.0; k++) {
        uint16_t e = d->tmp[k].id;
        if (find_link(d, e)) continue;
        if (now < d->retry_after[e] || d->near_since[e] == 0 || now - d->near_since[e] < MIN_NEIGHBOR) continue;
        float vx, vz;
        vel_of(&p->remotes[e].st, &vx, &vz);
        float rv = sqrtf((vx - mvx) * (vx - mvx) + (vz - mvz) * (vz - mvz));
        if (rv > MAX_REL_SPEED) continue;
        if (!budget_ok(n, d->nl)) {
            /* cupo lleno (C enlaces): un vecino de los C/2 más cercanos puede desplazar al enlace
               más flojo con vida ≥ 30 s cuyo vecino ya no esté entre los C más cercanos */
            int cap = n->cfg.direct_max < DIRECT_MAX ? n->cfg.direct_max : DIRECT_MAX;
            if (k >= (cap + 1) / 2) break;
            dlink_t* weak = NULL;
            for (int i = 0; i < d->nl; i++) {
                dlink_t* l = &d->l[i];
                if (l->state != DL_UP || now - l->t_up < MIN_LIFE || d->rank[l->id] < cap) continue;
                if (!weak || d->rank[l->id] > d->rank[weak->id]) weak = l;
            }
            if (!weak) break;
            drop_link(n, weak, 1, now);
            if (!budget_ok(n, d->nl)) break;
        }
        dlink_t* l = &d->l[d->nl++];
        memset(l, 0, sizeof *l);
        l->id = e;
        l->state = DL_OFFERED;
        l->mine = 1;
        link_random(n->link, (uint8_t*)&l->token, 4);
        l->t_start = now;
        d->tokens -= 1.0;
        d->attempts++;
        send_offer_or_answer(n, MSG_LINK_OFFER, e, l->token, 0);
    }
}

/* ----------------------------------------------------- reenvío en el SN */

/* Etapas 0 y 1 de LINK_OFFER / LINK_ANSWER. El SN agrega la dirección del peer que origina
 * (tal como la ve: es la que sirve para perforar su NAT) y la manda al SN del destinatario. */
int sn_forward_link(smnet_node_t* n, const smnet_addr_t* from, const smnet_hdr_t* h, rbuf_t* r) {
    sn_t* s = n->sn;
    uint8_t stage = r_u8(r);
    uint16_t src = r_u16(r), dst = r_u16(r);
    uint32_t token = r_u32(r);
    uint8_t nat = r_u8(r);
    uint8_t accept = h->type == MSG_LINK_ANSWER ? r_u8(r) : 0;
    smnet_addr_t a;
    if (stage == 1) addr_read(r, &a);
    else a = *from;
    if (r->err || src >= SMNET_MAX_PLAYERS || dst >= SMNET_MAX_PLAYERS) return 0;
    if (stage == 0 && (h->sender != src || !s->ents[src].hosted || !smnet_addr_eq(from, &s->ents[src].addr))) return 0;   /* solo de un alojado, por sí mismo */
    if (stage == 1) {
        int from_sn = 0;
        for (int k = 0; k < n->nsn; k++) if (smnet_addr_eq(from, &n->snlist[k].addr)) { from_sn = 1; break; }
        if (!from_sn) return 0;
    }
    const ent_t* de = &s->ents[dst];
    uint8_t buf[64];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    if (de->hosted) {
        node_hdr(n, &w, h->type, 0, 0);
        w_u8(&w, 2);
    } else if (stage == 0 && de->known && de->host_sn != n->id) {
        node_hdr(n, &w, h->type, 0, 0);
        w_u8(&w, 1);
    } else return 1;   /* no sabemos dónde está: el que ofreció vence por tiempo */
    w_u16(&w, src);
    w_u16(&w, dst);
    w_u32(&w, token);
    w_u8(&w, nat);
    if (h->type == MSG_LINK_ANSWER) w_u8(&w, accept);
    addr_write(&w, &a);
    if (w.err) return 0;
    if (de->hosted) node_send_id(n, dst, &de->addr, buf, w.len);
    else {
        for (int k = 0; k < n->nsn; k++)
            if (n->snlist[k].id == de->host_sn) { node_send_id(n, de->host_sn, &n->snlist[k].addr, buf, w.len); break; }
    }
    return 1;
}
