/* Mensajes de la aplicación (API v1, PROTOCOL.md §6.4): alcance área / todos / jugador / coordinador.
 *
 *   área, todos : emisor -> su SN primario -> sus alojados (área: los de esa área) y, una sola vez,
 *                 a los demás SN de la lista -> sus alojados. No hay más saltos: no hay bucles.
 *   jugador     : emisor -> coordinador -> destinatario (el coordinador tiene sesión con todos).
 *   coordinador : emisor -> coordinador.
 *
 * Cada salto va por el canal confiable del enlace. Límite de tasa en el origen y en el primer SN
 * (o en el coordinador): un jugador no puede inundar a los 1024.
 *
 * Cuerpo de APP_UP y APP_DOWN: [n u8] y n entradas
 *     [scope u8][flags u8][target u16][origin u16][mseq u16][area u16][len u16][datos]
 * Los SN agrupan por destino (cola por id que se vacía en cada smnet_node_update): con 1024
 * mandando a la vez, un mensaje confiable por entrada desbordaba la cola de la sesión (512). */
#include <stdlib.h>
#include <string.h>
#include "node_internal.h"

#define APPF_RELAYED 1

/* cubeta: `rate` por segundo, ráfaga `burst` */
static int bucket_take(app_bucket_t* b, double now, float rate, float burst) {
    if (b->t == 0) { b->t = now; b->tokens = burst; }
    b->tokens += (float)((now - b->t) / 1000.0) * rate;
    if (b->tokens > burst) b->tokens = burst;
    b->t = now;
    if (b->tokens < 1.0f) return 0;
    b->tokens -= 1.0f;
    return 1;
}
static int scope_ok(int scope) { return scope >= SMNET_TO_AREA && scope <= SMNET_TO_COORD; }
static int take_for(const smnet_node_t* n, app_bucket_t* bk, int scope, double now) {
    if (scope == SMNET_TO_ALL) return bucket_take(&bk[0], now, APP_RATE_ALL, APP_BURST_ALL);
    if (scope == SMNET_TO_AREA) return bucket_take(&bk[1], now, APP_RATE_AREA, APP_BURST_AREA);
    float r = n->cfg.app_rate_direct > 0 ? n->cfg.app_rate_direct : APP_RATE_DIRECT;
    return bucket_take(&bk[2], now, r, 2 * r);
}

typedef struct { uint8_t scope, flags; uint16_t target, origin, mseq, area, len; const uint8_t* data; } appmsg_t;

static int parse(rbuf_t* r, appmsg_t* m) {
    m->scope = r_u8(r); m->flags = r_u8(r);
    m->target = r_u16(r); m->origin = r_u16(r); m->mseq = r_u16(r); m->area = r_u16(r);
    m->len = r_u16(r);
    if (r->err || !scope_ok(m->scope) || m->len > SMNET_MAX_APP || r->pos + m->len > r->len) return -1;
    m->data = r->p + r->pos;
    r->pos += m->len;
    return 0;
}

static void w_entry(wbuf_t* w, const appmsg_t* m, uint8_t flags) {
    w_u8(w, m->scope); w_u8(w, flags);
    w_u16(w, m->target); w_u16(w, m->origin); w_u16(w, m->mseq); w_u16(w, m->area);
    w_u16(w, m->len);
    w_bytes(w, m->data, m->len);
}
#define ENTRY_HDR 12
#define APP_BUF (SMNET_HDR_SIZE + 1 + ENTRY_HDR + SMNET_MAX_APP)   /* cabecera + n + una entrada máxima */

static int build(smnet_node_t* n, uint8_t type, const appmsg_t* m, uint8_t flags, uint8_t* buf, int cap) {
    wbuf_t w = { buf, 0, cap, 0 };
    node_hdr(n, &w, type, 0, 0);
    w_u8(&w, 1);
    w_entry(&w, m, flags);
    return w.err ? 0 : w.len;
}

/* ---- colas de salida agrupadas por destino ---- */
typedef struct { smnet_addr_t addr; uint16_t len; uint8_t cnt; uint8_t buf[SMNET_MTU]; } appout_t;

static void out_flush_one(smnet_node_t* n, appout_t* o, uint16_t id) {
    if (!o || !o->cnt) return;
    o->buf[SMNET_HDR_SIZE] = o->cnt;
    node_send_id(n, id, &o->addr, o->buf, o->len);
    o->cnt = 0;
}

static void out_add(smnet_node_t* n, int relay, uint16_t id, const smnet_addr_t* addr, const appmsg_t* m, uint8_t flags) {
    void** ptab = relay ? &n->app_out_relay : &n->app_out_down;
    if (!*ptab) { *ptab = calloc(SMNET_MAX_PLAYERS, sizeof(appout_t*)); if (!*ptab) return; }
    appout_t** slot = (appout_t**)*ptab + id;
    if (!*slot) { *slot = calloc(1, sizeof(appout_t)); if (!*slot) return; }
    appout_t* o = *slot;
    int need = ENTRY_HDR + m->len;
    if (o->cnt && (o->len + need > SMNET_MTU || o->cnt == 255)) out_flush_one(n, o, id);
    if (!o->cnt) {
        wbuf_t w = { o->buf, 0, SMNET_MTU, 0 };
        node_hdr(n, &w, relay ? MSG_APP_UP : MSG_APP_DOWN, 0, 0);
        w_u8(&w, 0);
        o->len = (uint16_t)w.len;
        o->addr = *addr;
    }
    wbuf_t w = { o->buf, o->len, SMNET_MTU, 0 };
    w_entry(&w, m, flags);
    if (w.err) return;
    o->len = (uint16_t)w.len;
    o->cnt++;
}

void app_flush(smnet_node_t* n) {
    for (int r = 0; r < 2; r++) {
        appout_t** tab = (appout_t**)(r ? n->app_out_relay : n->app_out_down);
        if (!tab) continue;
        for (int i = 0; i < SMNET_MAX_PLAYERS; i++) if (tab[i] && tab[i]->cnt) out_flush_one(n, tab[i], (uint16_t)i);
    }
}

void app_free(smnet_node_t* n) {
    for (int r = 0; r < 2; r++) {
        appout_t** tab = (appout_t**)(r ? n->app_out_relay : n->app_out_down);
        if (!tab) continue;
        for (int i = 0; i < SMNET_MAX_PLAYERS; i++) free(tab[i]);
        free(tab);
    }
    free(n->app_last); free(n->app_bits);
    n->app_out_relay = n->app_out_down = NULL; n->app_last = NULL; n->app_bits = NULL;
}

static void deliver(smnet_node_t* n, const appmsg_t* m, double now) {
    n->stats.app_rx++;
    if (n->on_msg) n->on_msg(n->user, m->origin, (smnet_scope_t)m->scope, m->data, m->len, now);
}

/* ------------------------------------------------------------------ API */

int smnet_node_send_msg(smnet_node_t* n, smnet_scope_t scope, uint16_t target, const uint8_t* data, int len) {
    peer_t* p = &n->peer;
    if (!scope_ok(scope) || len < 0 || len > SMNET_MAX_APP) return -2;
    if (n->id >= SMNET_MAX_PLAYERS || p->status != SMNET_PEER_CONNECTED) return -1;
    if (scope == SMNET_TO_PLAYER && target >= SMNET_MAX_PLAYERS) return -2;
    if (!take_for(n, n->app_bucket, scope, n->now)) { n->stats.app_rate_limited++; return -3; }
    appmsg_t m = { (uint8_t)scope, 0, target, n->id, ++n->app_mseq, p->area, (uint16_t)len, data };
    uint8_t buf[APP_BUF];
    int bl = build(n, MSG_APP_UP, &m, 0, buf, sizeof buf);
    if (!bl) return -2;
    n->stats.app_tx++;
    if (scope == SMNET_TO_AREA || scope == SMNET_TO_ALL) {
        if (p->primary.id == SMNET_NO_ID) return -1;
        node_send_id(n, p->primary.id, &p->primary.addr, buf, bl);
    } else node_send_coord(n, buf, bl);
    return 0;
}

void smnet_node_set_app_callbacks(smnet_node_t* n, smnet_on_msg_fn on_msg, smnet_on_player_fn on_player, smnet_on_owner_fn on_owner) {
    n->on_msg = on_msg; n->on_player = on_player; n->on_owner = on_owner;
}

/* ------------------------------------------------------------- recepción */

/* SN: entrega a los alojados (salvo al que lo originó) */
static void sn_fanout(smnet_node_t* n, const appmsg_t* m) {
    sn_t* s = n->sn;
    for (int e = 0; e < SMNET_MAX_PLAYERS; e++) {
        ent_t* ee = &s->ents[e];
        if (!ee->hosted || e == m->origin) continue;
        if (m->scope == SMNET_TO_AREA && ee->area != m->area) continue;
        out_add(n, 0, (uint16_t)e, &ee->addr, m, 0);
    }
}

static int is_listed_sn(const smnet_node_t* n, uint16_t id) { return node_is_sn_or_recent(n, id); }

static int up_one(smnet_node_t* n, const smnet_hdr_t* h, rbuf_t* r, int local, double now) {
    appmsg_t m;
    if (parse(r, &m) != 0) return -1;
    int relayed = (m.flags & APPF_RELAYED) != 0;
    /* el origen tiene que ser quien lo manda (salvo el reenvío entre SN) */
    if (!local && !relayed && m.origin != h->sender) { n->stats.unauthorized++; return 0; }
    if (relayed && (local || !is_listed_sn(n, h->sender))) { n->stats.unauthorized++; return 0; }
    if (m.scope == SMNET_TO_COORD || m.scope == SMNET_TO_PLAYER) {
        if (!n->co || n->co->role != SMNET_COORD_ACTIVE || m.origin >= SMNET_MAX_PLAYERS) return 0;
        cnode_t* o = &n->co->nodes[m.origin];
        if (!o->used) return 0;
        if (!local && !take_for(n, o->app_bucket, m.scope, now)) { n->stats.app_rate_limited++; return 1; }
        if (m.scope == SMNET_TO_COORD) { deliver(n, &m, now); return 1; }
        if (m.target >= SMNET_MAX_PLAYERS || !n->co->nodes[m.target].used) return 1;
        if (m.target == n->id) { deliver(n, &m, now); return 1; }
        uint8_t buf[APP_BUF];
        int bl = build(n, MSG_APP_DOWN, &m, 0, buf, sizeof buf);
        if (bl) node_send_id(n, m.target, &n->co->nodes[m.target].addr, buf, bl);
        return 1;
    }
    /* área / todos: lo atiende un SN */
    if (!n->sn || m.origin >= SMNET_MAX_PLAYERS) return 0;
    if (!relayed) {
        ent_t* oe = &n->sn->ents[m.origin];
        if (!local && !oe->hosted) return 0;   /* solo de nuestros alojados */
        if (!local && !take_for(n, oe->app_bucket, m.scope, now)) { n->stats.app_rate_limited++; return 1; }
        /* a los demás SN, una vez */
        for (int i = 0; i < n->nsn; i++) {
            if (n->snlist[i].id == n->id || node_is_banned(n, n->snlist[i].id)) continue;
            out_add(n, 1, n->snlist[i].id, &n->snlist[i].addr, &m, APPF_RELAYED);
        }
    }
    sn_fanout(n, &m);
    return 1;
}

static int down_one(smnet_node_t* n, const smnet_hdr_t* h, rbuf_t* r, int local, double now) {
    appmsg_t m;
    if (parse(r, &m) != 0) return -1;
    /* solo de un SN de la lista o del coordinador vigente (o de nosotros mismos) */
    if (!local && !((n->have_coord && h->sender == n->coord_id) || is_listed_sn(n, h->sender))) { n->stats.unauthorized++; return 0; }
    if (!local && node_is_banned(n, h->sender)) return 0;
    if (m.origin == n->id || m.origin >= SMNET_MAX_PLAYERS) return 1;
    /* duplicados: durante un traspaso un peer puede estar alojado en dos SN a la vez */
    if (!n->app_last) {
        n->app_last = calloc(SMNET_MAX_PLAYERS, sizeof(uint16_t));
        n->app_bits = calloc(SMNET_MAX_PLAYERS, sizeof(uint32_t));
        if (!n->app_last || !n->app_bits) return 1;
    }
    uint16_t* last = &n->app_last[m.origin];
    uint32_t* bits = &n->app_bits[m.origin];
    int16_t d = (int16_t)(m.mseq - *last);
    if (*bits == 0 && *last == 0) { *last = m.mseq; *bits = 1; }
    else if (d > 0) { *bits = d >= 32 ? 1u : ((*bits << d) | 1u); *last = m.mseq; }
    else {
        int k = -d;
        if (k >= 32) { *last = m.mseq; *bits = 1; }       /* muy atrás: el emisor reinició la numeración */
        else if ((*bits >> k) & 1u) { n->stats.app_dup++; return 1; }
        else *bits |= 1u << k;
    }
    deliver(n, &m, now);
    return 1;
}

int app_on_up(smnet_node_t* n, const smnet_hdr_t* h, rbuf_t* r, int local, double now) {
    int cnt = r_u8(r), ok = 0;
    for (int i = 0; i < cnt && !r->err; i++) { int rc = up_one(n, h, r, local, now); if (rc < 0) return ok; ok |= rc; }
    return ok;
}

int app_on_down(smnet_node_t* n, const smnet_hdr_t* h, rbuf_t* r, int local, double now) {
    int cnt = r_u8(r), ok = 0;
    for (int i = 0; i < cnt && !r->err; i++) { int rc = down_one(n, h, r, local, now); if (rc < 0) return ok; ok |= rc; }
    return ok;
}
