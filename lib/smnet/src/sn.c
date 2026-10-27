/* Rol supernodo (§8): aloja a sus peers, reenvía estado a los demás SN (base 2 Hz para todo
 * lo alojado + suscripciones a 10/30 Hz para lo que piden sus viewers) y arma para cada
 * peer alojado el lote por cercanía con priority accumulator. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "node_internal.h"

#define UDP_IP_OVH 28
#define TIER_10 1
#define TIER_30 2
#define WANT_RICH 0x80

/* ------------------------------------------------------------- memoria */

static view_t* view_new(int self_id, double seedmix) {
    view_t* v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->cand = calloc(SMNET_MAX_PLAYERS, sizeof(uint16_t));
    v->cand_hz = calloc(SMNET_MAX_PLAYERS, sizeof(float));
    v->prio = calloc(SMNET_MAX_PLAYERS, sizeof(float));
    v->sent_seq = calloc(SMNET_MAX_PLAYERS, 1);
    v->sent_valid = calloc(SMNET_MAX_PLAYERS, 1);
    v->blob_sent = calloc(SMNET_MAX_PLAYERS, sizeof(uint16_t));
    if (!v->cand || !v->cand_hz || !v->prio || !v->sent_seq || !v->sent_valid || !v->blob_sent) {
        free(v->cand); free(v->cand_hz); free(v->prio); free(v->sent_seq); free(v->sent_valid); free(v->blob_sent);
        free(v);
        return NULL;
    }
    /* fase pseudoaleatoria del priority accumulator (normativo, §8.2) */
    uint32_t mix = (uint32_t)seedmix;
    for (int e = 0; e < SMNET_MAX_PLAYERS; e++) {
        uint32_t h = (uint32_t)(self_id * 2654435761u) ^ (uint32_t)(e * 2246822519u) ^ mix;
        h ^= h >> 15; h *= 2654435761u; h ^= h >> 13;
        v->prio[e] = (float)(h & 0xFFFF) / 65536.0f;
    }
    return v;
}

static void view_free(view_t* v) {
    if (!v) return;
    free(v->cand); free(v->cand_hz); free(v->prio); free(v->sent_seq); free(v->sent_valid); free(v->blob_sent);
    free(v);
}

static int other_alloc(other_t* o) {
    o->sub = calloc(SMNET_MAX_PLAYERS, 1);
    o->prio = calloc(SMNET_MAX_PLAYERS, sizeof(float));
    o->sent_seq = calloc(SMNET_MAX_PLAYERS, 1);
    o->sent_valid = calloc(SMNET_MAX_PLAYERS, 1);
    o->blob_sent = calloc(SMNET_MAX_PLAYERS, sizeof(uint16_t));
    if (!o->sub || !o->prio || !o->sent_seq || !o->sent_valid || !o->blob_sent) return -1;
    for (int e = 0; e < SMNET_MAX_PLAYERS; e++) {
        uint32_t h = (uint32_t)(o->id * 2654435761u) ^ (uint32_t)(e * 40503u);
        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
        o->prio[e] = (float)(h & 0xFFFF) / 65536.0f;
    }
    return 0;
}

static void other_free(other_t* o) {
    free(o->sub); free(o->prio); free(o->sent_seq); free(o->sent_valid); free(o->blob_sent);
    memset(o, 0, sizeof *o);
}

sn_t* sn_new(smnet_node_t* n, double now) {
    (void)n;
    sn_t* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->ents = calloc(SMNET_MAX_PLAYERS, sizeof(ent_t));
    s->want = calloc(SMNET_MAX_PLAYERS, 1);
    s->tmp = calloc(SMNET_MAX_PLAYERS, sizeof *s->tmp);
    s->elig = calloc(SMNET_MAX_PLAYERS, sizeof(uint16_t));
    if (!s->ents || !s->want || !s->tmp || !s->elig) { sn_free(s); return NULL; }
    s->next_tick = now;
    s->last_report = now - 1e9;
    s->last_subscribe = now;
    s->start_ms = now;
    s->window_start = now;
    return s;
}

void sn_free(sn_t* s) {
    if (!s) return;
    if (s->ents) for (int i = 0; i < SMNET_MAX_PLAYERS; i++) view_free(s->ents[i].v);
    for (int i = 0; i < s->nothers; i++) other_free(&s->others[i]);
    free(s->ents); free(s->want); free(s->tmp); free(s->elig);
    free(s);
}

/* Sincroniza la lista de otros SN con la del nodo, conservando el estado de los que siguen. */
void sn_sync_others(smnet_node_t* n) {
    sn_t* s = n->sn;
    other_t keep[SMNET_MAX_SN];
    int nk = 0;
    for (int i = 0; i < n->nsn && nk < SMNET_MAX_SN; i++) {
        const sninfo_t* si = &n->snlist[i];
        if (si->id == n->id) continue;
        int found = -1;
        for (int j = 0; j < s->nothers; j++) if (s->others[j].id == si->id) { found = j; break; }
        if (found >= 0) {
            keep[nk] = s->others[found];
            s->others[found].id = SMNET_NO_ID;   /* marcado como movido */
            s->others[found].sub = NULL;
        } else {
            memset(&keep[nk], 0, sizeof keep[nk]);
            keep[nk].id = si->id;
            if (other_alloc(&keep[nk]) != 0) { other_free(&keep[nk]); continue; }
        }
        keep[nk].addr = si->addr;
        keep[nk].flags = si->flags;
        nk++;
    }
    for (int j = 0; j < s->nothers; j++) if (s->others[j].sub) other_free(&s->others[j]);
    memcpy(s->others, keep, sizeof(other_t) * (size_t)nk);
    s->nothers = nk;
}

int sn_hosted_count(const sn_t* s, double now, double expire) {
    int c = 0;
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++)
        if (s->ents[i].hosted && now - s->ents[i].last_up_ms <= expire) c++;
    return c;
}

/* ------------------------------------------------------------- recepción */

static void reply_simple(smnet_node_t* n, uint16_t to_id, const smnet_addr_t* to, uint8_t type, uint8_t flags, const uint8_t* extra, int elen) {
    uint8_t buf[32];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    node_hdr(n, &w, type, flags, 0);
    if (extra) w_bytes(&w, extra, elen);
    node_send_id(n, to_id, to, buf, w.len);
}

static other_t* find_other(sn_t* s, uint16_t id) {
    for (int i = 0; i < s->nothers; i++) if (s->others[i].id == id) return &s->others[i];
    return NULL;
}

int sn_receive(smnet_node_t* n, const smnet_addr_t* from, const smnet_hdr_t* h, rbuf_t* r, double now) {
    sn_t* s = n->sn;
    uint16_t id = h->sender;
    switch (h->type) {
    case MSG_STATE_UP: {
        if (id >= SMNET_MAX_PLAYERS) return 0;
        uint16_t area = r_u16(r);
        smnet_state_t st;
        state_read(r, &st);
        if (r->err) return 0;
        ent_t* e = &s->ents[id];
        uint8_t seq = (uint8_t)h->seq;
        if (e->hosted && now - e->last_up_ms < 1000 && (uint8_t)(seq - e->seq8) > 128) return 1;   /* desordenado */
        if (!e->hosted || e->area != area) {
            s->dirty = 1;
            /* viewer nuevo (entrada, failover, traspaso): suscribirse ya a sus vecinos de otros SN,
               sin esperar la renovación de 250 ms (si no, el primer dato nuevo llega con la base
               de 2 Hz: hasta 500 ms más de corte después de un failover) */
            s->sub_now = 1;
            if (!e->hosted) e->arrived = 1;
        }
        if (!e->v) e->v = view_new(n->id, now);
        e->known = 1;
        e->hosted = 1;
        e->host_sn = n->id;
        e->addr = *from;
        e->area = area;
        e->st = st;
        e->seq8 = seq;
        e->last_ms = e->last_up_ms = now;
        if (h->flags & UP_HAS_BLOB) {
            uint16_t bl = r_u16(r);
            if (bl <= SMNET_MAX_BLOB) {
                r_bytes(r, e->blob, bl);
                if (!r->err) { e->blob_len = bl; e->blob_ver++; }
            }
        }
        return 1;
    }
    case MSG_LEAVE:
        if (id < SMNET_MAX_PLAYERS && s->ents[id].hosted) { s->ents[id].hosted = 0; s->dirty = 1; }
        return 1;
    case MSG_HEARTBEAT:
        if (id < SMNET_MAX_PLAYERS) reply_simple(n, id, from, MSG_HEARTBEAT, HB_FROM_SN, NULL, 0);
        return 1;
    case MSG_DIRECT_SET: {
        if (id >= SMNET_MAX_PLAYERS || !s->ents[id].hosted || !s->ents[id].v) return 0;
        view_t* vw = s->ents[id].v;
        int cnt = r_u8(r);
        if (cnt > DIRECT_MAX) cnt = DIRECT_MAX;
        int changed = cnt != vw->ndset;
        for (int i = 0; i < cnt && !r->err; i++) {
            uint16_t e = r_u16(r);
            if (vw->dset[i] != e) changed = 1;
            vw->dset[i] = e;
        }
        if (r->err) return 0;
        vw->ndset = cnt;
        vw->dset_until = now + 2500;
        if (changed) s->dirty = 1;
        return 1;
    }
    case MSG_INTRO_REQ: {
        /* un peer alojado quiere hablar con otros SN: les pasamos su dirección como la vemos */
        int cnt = r_u8(r);
        for (int i = 0; i < cnt && !r->err; i++) {
            uint16_t sid = r_u16(r);
            if (r->err) break;
            for (int k = 0; k < n->nsn; k++)
                if (n->snlist[k].id == sid && sid != n->id) node_send_intro(n, sid, &n->snlist[k].addr, id, from);
        }
        return !r->err;
    }
    case MSG_PING: {
        uint8_t t[4];
        r_bytes(r, t, 4);
        if (r->err) return 0;
        reply_simple(n, id, from, MSG_PONG, 0, t, 4);
        return 1;
    }
    case MSG_STATE_RELAY: {
        int cnt = r_u8(r);
        for (int i = 0; i < cnt && !r->err; i++) {
            uint16_t eid = r_u16(r);
            uint8_t seq = r_u8(r);
            uint16_t area = r_u16(r);
            smnet_state_t st;
            state_read(r, &st);
            if (r->err || eid >= SMNET_MAX_PLAYERS) break;
            ent_t* e = &s->ents[eid];
            if (e->hosted && now - e->last_up_ms <= n->cfg.sn.hosted_expire_ms) continue;   /* lo tengo de primera mano */
            int fresh = now - e->last_ms < 1000;
            if (e->known && fresh && e->host_sn == id && (uint8_t)(seq - e->seq8) > 128) continue;
            if (e->known && fresh && e->host_sn == id && seq == e->seq8) continue;
            if (!e->known || e->area != area) s->dirty = 1;
            if (e->known && e->host_sn != id && s->want[eid]) s->sub_now = 1;   /* se mudó: re-suscribir ya */
            e->known = 1;
            e->hosted = 0;
            e->host_sn = id;
            e->area = area;
            e->st = st;
            e->seq8 = seq;
            e->last_ms = now;
            n->stats.sn_relay_entities++;
        }
        return 1;
    }
    case MSG_SUBSCRIBE: {
        other_t* o = find_other(s, id);
        if (!o) return 0;
        int cnt = r_u16(r);
        uint8_t part = (uint8_t)(h->flags & 0x0F);
        if (part == 0) memset(o->sub, 0, SMNET_MAX_PLAYERS);   /* primera parte: reemplaza */
        for (int i = 0; i < cnt && !r->err; i++) {
            uint16_t eid = r_u16(r);
            uint8_t code = r_u8(r);
            if (!r->err && eid < SMNET_MAX_PLAYERS) o->sub[eid] = code;
        }
        o->sub_ms = now;
        return 1;
    }
    case MSG_RELAY_FULL: {
        int cnt = r_u8(r);
        for (int i = 0; i < cnt && !r->err; i++) {
            uint16_t eid = r_u16(r);
            uint8_t seq = r_u8(r);
            uint16_t len = r_u16(r);
            if (len > SMNET_MAX_BLOB || eid >= SMNET_MAX_PLAYERS) { r->err = 1; break; }
            ent_t* e = &s->ents[eid];
            if (e->hosted) { r->pos += len; if (r->pos > r->len) r->err = 1; continue; }
            r_bytes(r, e->blob, len);
            if (r->err) break;
            e->blob_len = len;
            e->blob_ver++;
            (void)seq;
        }
        return 1;
    }
    }
    return 0;
}

/* ----------------------------------------------------------------- interés */

static int cmp_de(const void* a, const void* b) {
    float x = ((const struct de*)a)->d, y = ((const struct de*)b)->d;
    return (x > y) - (x < y);
}

static void select_k(struct de* a, int n, int k) {
    int lo = 0, hi = n - 1;
    if (k <= 0 || k >= n) return;
    while (lo < hi) {
        float pivot = a[(lo + hi) / 2].d;
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i].d < pivot) i++;
            while (a[j].d > pivot) j--;
            if (i <= j) { struct de t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (k - 1 <= j) hi = j;
        else if (k - 1 >= i) lo = i;
        else break;
    }
}

static int alive_ent(const smnet_node_t* n, const ent_t* e, double now) {
    if (!e->known) return 0;
    if (e->hosted) return now - e->last_up_ms <= n->cfg.sn.hosted_expire_ms;
    return now - e->last_ms <= n->cfg.sn.remote_stale_ms;
}

static void rerank(smnet_node_t* n, double now) {
    sn_t* s = n->sn;
    const smnet_sn_cfg_t* c = &n->cfg.sn;
    memset(s->want, 0, SMNET_MAX_PLAYERS);
    for (int v = 0; v < SMNET_MAX_PLAYERS; v++) {
        ent_t* ev = &s->ents[v];
        if (!ev->hosted || !ev->v) continue;
        view_t* vw = ev->v;
        uint16_t prevfull[SMNET_MAX_FULL]; int nprev = vw->nfull;
        if (nprev > SMNET_MAX_FULL) nprev = SMNET_MAX_FULL;
        memcpy(prevfull, vw->full, sizeof prevfull[0] * (size_t)nprev);
        vw->ncand = 0;
        vw->nfull = 0;
        if (now - ev->last_up_ms > c->hosted_expire_ms) continue;
        int cnt = 0;
        for (int e = 0; e < SMNET_MAX_PLAYERS; e++) {
            ent_t* ee = &s->ents[e];
            if (e == v || !alive_ent(n, ee, now) || ee->area != ev->area) continue;
            float dx = ee->st.x - ev->st.x, dy = ee->st.y - ev->st.y, dz = ee->st.z - ev->st.z;
            s->tmp[cnt].d = dx * dx + dy * dy + dz * dz;
            s->tmp[cnt].id = (uint16_t)e;
            cnt++;
        }
        int k1 = c->tier_k[0] < cnt ? c->tier_k[0] : cnt;
        int k2 = c->tier_k[1] < cnt ? c->tier_k[1] : cnt;
        int m2 = c->tier_k[1] + c->tier_k[1] / 4; if (m2 > cnt) m2 = cnt;
        int m1 = c->tier_k[0] + c->tier_k[0] / 2; if (m1 > cnt) m1 = cnt;
        select_k(s->tmp, cnt, m2);
        select_k(s->tmp, m2, k2);
        select_k(s->tmp, k2, m1);
        select_k(s->tmp, m1, k1);
        qsort(s->tmp, (size_t)k1, sizeof *s->tmp, cmp_de);
        float r2 = c->full_radius * c->full_radius;
        /* suscripción con margen: se pide a 30 Hz hasta 1,5×k1 y a 10 Hz hasta 1,25×k2, para que
           un vecino que entra al grupo cercano ya llegue a buena tasa mientras se renueva */
        int s1 = k1 + k1 / 2, s2 = k2 + k2 / 4;
        int use_dset = vw->ndset > 0 && now < vw->dset_until;
        /* histéresis de los ricos: el que ya tenía estado rico lo mantiene mientras siga entre los
           1,5×k1 más cercanos (con cientos amontonados, el puesto k1 cambia a cada vuelta y el
           viewer veía rotar sus slots); los lugares que quedan, a los más cercanos */
        for (int k = 0; k < m1 && vw->nfull < c->full_k; k++) {
            uint16_t id = s->tmp[k].id;
            if (s->tmp[k].d > r2) continue;
            for (int j = 0; j < nprev; j++) if (prevfull[j] == id) { vw->full[vw->nfull++] = id; s->want[id] |= WANT_RICH; break; }
        }
        for (int k = 0; k < cnt; k++) {
            uint16_t id = s->tmp[k].id;
            vw->cand[k] = id;
            uint8_t tier = k < k1 ? TIER_30 : (k < k2 ? TIER_10 : 0);
            uint8_t stier = k < s1 ? TIER_30 : (k < s2 ? TIER_10 : 0);
            if (use_dset) {
                int direct = 0;
                for (int j = 0; j < vw->ndset; j++) if (vw->dset[j] == id) { direct = 1; break; }
                if (direct) {
                    /* le llega por atajo directo: 2 Hz por el SN (respaldo y medición) y sin rico */
                    vw->cand_hz[k] = c->tier_hz[2];
                    continue;
                }
            }
            vw->cand_hz[k] = tier == TIER_30 ? c->tier_hz[0] : (tier == TIER_10 ? c->tier_hz[1] : c->tier_hz[2]);
            uint8_t* w = &s->want[id];
            if ((*w & 0x7F) < stier) *w = (uint8_t)((*w & WANT_RICH) | stier);
            if (k < k1 && vw->nfull < c->full_k && s->tmp[k].d <= r2) {
                int dup = 0;
                for (int j = 0; j < vw->nfull; j++) if (vw->full[j] == id) { dup = 1; break; }
                if (!dup) { vw->full[vw->nfull++] = id; *w |= WANT_RICH; }
            }
        }
        vw->ncand = cnt;
        /* lo que el viewer verifica, a 30 Hz desde su SN de origen (así llegan también sus pruebas) */
        for (int k = 0; k < vw->nvset; k++) {
            uint16_t id = vw->vset[k];
            if (id >= SMNET_MAX_PLAYERS) continue;
            uint8_t* w = &s->want[id];
            if ((*w & 0x7F) < TIER_30) *w = (uint8_t)((*w & WANT_RICH) | TIER_30);
        }
    }
}

static const float* g_prio;
static int cmp_prio(const void* a, const void* b) {
    float x = g_prio[*(const uint16_t*)a], y = g_prio[*(const uint16_t*)b];
    return (x < y) - (x > y);
}

static int fresh_for(const ent_t* e, const uint8_t* sent_seq, const uint8_t* sent_valid, uint16_t id) {
    return !sent_valid[id] || sent_seq[id] != e->seq8;
}

static void send_batches(smnet_node_t* n, uint16_t v, double now) {
    sn_t* s = n->sn;
    const smnet_sn_cfg_t* c = &n->cfg.sn;
    ent_t* ev = &s->ents[v];
    view_t* vw = ev->v;
    int ne = 0;
    /* acumulador en fases: una entidad de f Hz suma f/30·P cada P ticks (P = 30/f redondeado),
       con la fase desfasada por su posición k. Misma tasa promedio que sumar en cada tick,
       pero las de 10 Hz se tocan 1 de cada 3 ticks y las de 2 Hz 1 de cada 15. */
    for (int k = 0; k < vw->ncand; k++) {
        float hz = vw->cand_hz[k];
        int period = hz >= 29.f ? 1 : (int)(30.0f / hz + 0.5f);
        if (period > 1 && (int)((s->tick + (uint16_t)k) % (uint16_t)period) != 0) continue;
        uint16_t e = vw->cand[k];
        ent_t* ee = &s->ents[e];
        if (!alive_ent(n, ee, now)) continue;
        float* pr = &vw->prio[e];
        *pr += hz / 30.0f * (float)period;
        if (*pr > 3.0f) *pr = 3.0f;
        if (*pr >= 1.0f && fresh_for(ee, vw->sent_seq, vw->sent_valid, e)) s->elig[ne++] = e;
    }
    int sent_any = 0;
    if (ne > 0) {
        const int per_dgram = (SMNET_MTU - SMNET_HDR_SIZE - 1) / SMNET_ENTITY_SIZE;
        double budget = c->down_budget_kbps * 1000.0 / 8.0 / 30.0;
        int fit = 0, dgrams = 0;
        while (fit < ne && dgrams < c->max_datagrams_per_tick) {
            int room = (int)((budget - (UDP_IP_OVH + SMNET_HDR_SIZE + 1)) / SMNET_ENTITY_SIZE);
            if (room <= 0) break;
            if (room > per_dgram) room = per_dgram;
            int take = ne - fit < room ? ne - fit : room;
            fit += take;
            budget -= UDP_IP_OVH + SMNET_HDR_SIZE + 1 + take * SMNET_ENTITY_SIZE;
            dgrams++;
        }
        if (fit < ne) {
            g_prio = vw->prio;
            qsort(s->elig, (size_t)ne, sizeof(uint16_t), cmp_prio);
            n->stats.sn_entities_cut += (uint64_t)(ne - fit);
        }
        uint8_t buf[SMNET_MTU];
        int i = 0;
        while (i < fit) {
            wbuf_t w = { buf, 0, sizeof buf, 0 };
            node_hdr(n, &w, MSG_STATE_BATCH, 0, ++vw->out_seq);
            int cnt = fit - i < per_dgram ? fit - i : per_dgram;
            int cpos = w.len, written = 0;
            w_u8(&w, (uint8_t)cnt);
            for (int k = 0; k < cnt; k++, i++) {
                uint16_t e = s->elig[i];
                ent_t* ee = &s->ents[e];
                if (n->cfg.sn.evil_drop > 0 && sn_evil_drop(n, v, e, ee->seq8)) {   /* SOLO PRUEBAS */
                    vw->prio[e] -= 1.0f; vw->sent_seq[e] = ee->seq8; vw->sent_valid[e] = 1;
                    continue;
                }
                written++;
                w_u16(&w, e);
                w_u8(&w, ee->seq8);
                if (n->cfg.sn.evil_alter) { smnet_state_t bad = ee->st; bad.x += 3000; state_write(&w, &bad); }   /* SOLO PRUEBAS */
                else state_write(&w, &ee->st);
                vw->prio[e] -= 1.0f;
                vw->sent_seq[e] = ee->seq8;
                vw->sent_valid[e] = 1;
            }
            buf[cpos] = (uint8_t)written;
            node_send_id(n, v, &ev->addr, buf, w.len);
            sent_any = 1;
        }
        n->stats.sn_entities_sent += (uint64_t)fit;
    }
    /* estado rico de los vecinos inmediatos */
    double fb = c->full_budget_kbps * 1000.0 / 8.0 / 30.0;
    uint8_t buf[SMNET_MTU];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    int cpos = -1, cnt = 0;
    for (int k = 0; k < vw->nfull; k++) {
        uint16_t e = vw->full[k];
        ent_t* ee = &s->ents[e];
        if (ee->blob_len == 0 || vw->blob_sent[e] == ee->blob_ver) continue;
        int need = 5 + ee->blob_len;
        if (cpos >= 0 && w.len + need > (int)sizeof buf) {
            buf[cpos] = (uint8_t)cnt;
            node_send_id(n, v, &ev->addr, buf, w.len);
            sent_any = 1;
            w.len = 0; cpos = -1; cnt = 0;
        }
        if (cpos < 0) {
            if (fb < UDP_IP_OVH + SMNET_HDR_SIZE + 1 + need) break;
            node_hdr(n, &w, MSG_STATE_FULL, 0, ++vw->out_seq);
            cpos = w.len;
            w_u8(&w, 0);
            fb -= UDP_IP_OVH + SMNET_HDR_SIZE + 1;
        }
        if (fb < need) break;
        w_u16(&w, e);
        w_u8(&w, ee->seq8);
        w_u16(&w, ee->blob_len);
        w_bytes(&w, ee->blob, ee->blob_len);
        fb -= need;
        vw->blob_sent[e] = ee->blob_ver;
        cnt++;
        n->stats.sn_blobs_sent++;
    }
    if (cpos >= 0 && cnt > 0) {
        buf[cpos] = (uint8_t)cnt;
        node_send_id(n, v, &ev->addr, buf, w.len);
        sent_any = 1;
    }
    if (sent_any) vw->last_tx_ms = now;
    else if (now - vw->last_tx_ms >= 250) {
        uint8_t hb[SMNET_HDR_SIZE];
        wbuf_t hw = { hb, 0, sizeof hb, 0 };
        node_hdr(n, &hw, MSG_HEARTBEAT, HB_FROM_SN, ++vw->out_seq);
        node_send_id(n, v, &ev->addr, hb, hw.len);
        vw->last_tx_ms = now;
    }
}

/* Reenvío a los demás SN: todo lo alojado a la tasa base, más lo suscripto. */
static void send_relays(smnet_node_t* n, double now) {
    sn_t* s = n->sn;
    const smnet_sn_cfg_t* c = &n->cfg.sn;
    const int per_dgram = (SMNET_MTU - SMNET_HDR_SIZE - 1) / SMNET_RELAY_SIZE;
    uint8_t buf[SMNET_MTU];
    for (int oi = 0; oi < s->nothers; oi++) {
        other_t* o = &s->others[oi];
        int subs_ok = now - o->sub_ms <= c->subscribe_ttl_ms;
        int ne = 0;
        for (int e = 0; e < SMNET_MAX_PLAYERS; e++) {
            ent_t* ee = &s->ents[e];
            if (!ee->hosted || now - ee->last_up_ms > c->hosted_expire_ms) continue;
            float hz = c->relay_base_hz;
            if (subs_ok) {
                uint8_t t = o->sub[e] & 0x7F;
                if (t == TIER_30) hz = c->tier_hz[0];
                else if (t == TIER_10) hz = c->tier_hz[1];
            }
            float* pr = &o->prio[e];
            *pr += hz / 30.0f;
            if (*pr > 3.0f) *pr = 3.0f;
            /* recién llegado (failover, traspaso): que todos los SN sepan ya dónde está y tengan
               un estado nuevo, sin esperar la base de 2 Hz */
            if (ee->arrived && *pr < 1.0f) *pr = 1.0f;
            if (*pr >= 1.0f && fresh_for(ee, o->sent_seq, o->sent_valid, (uint16_t)e)) s->elig[ne++] = (uint16_t)e;
        }
        int i = 0;
        while (i < ne) {
            wbuf_t w = { buf, 0, sizeof buf, 0 };
            node_hdr(n, &w, MSG_STATE_RELAY, 0, ++o->out_seq);
            int cnt = ne - i < per_dgram ? ne - i : per_dgram;
            w_u8(&w, (uint8_t)cnt);
            for (int k = 0; k < cnt; k++, i++) {
                uint16_t e = s->elig[i];
                ent_t* ee = &s->ents[e];
                w_u16(&w, e);
                w_u8(&w, ee->seq8);
                w_u16(&w, ee->area);
                state_write(&w, &ee->st);
                o->prio[e] -= 1.0f;
                o->sent_seq[e] = ee->seq8;
                o->sent_valid[e] = 1;
            }
            node_send_id(n, o->id, &o->addr, buf, w.len);
        }
        /* estado rico pedido */
        if (!subs_ok) continue;
        wbuf_t w = { buf, 0, sizeof buf, 0 };
        int cpos = -1, cnt = 0;
        for (int e = 0; e < SMNET_MAX_PLAYERS; e++) {
            ent_t* ee = &s->ents[e];
            if (!ee->hosted || !(o->sub[e] & WANT_RICH) || ee->blob_len == 0 || o->blob_sent[e] == ee->blob_ver) continue;
            int need = 5 + ee->blob_len;
            if (cpos >= 0 && w.len + need > (int)sizeof buf) {
                buf[cpos] = (uint8_t)cnt;
                node_send_id(n, o->id, &o->addr, buf, w.len);
                w.len = 0; cpos = -1; cnt = 0;
            }
            if (cpos < 0) {
                node_hdr(n, &w, MSG_RELAY_FULL, 0, ++o->out_seq);
                cpos = w.len;
                w_u8(&w, 0);
            }
            w_u16(&w, (uint16_t)e);
            w_u8(&w, ee->seq8);
            w_u16(&w, ee->blob_len);
            w_bytes(&w, ee->blob, ee->blob_len);
            o->blob_sent[e] = ee->blob_ver;
            cnt++;
        }
        if (cpos >= 0 && cnt > 0) { buf[cpos] = (uint8_t)cnt; node_send_id(n, o->id, &o->addr, buf, w.len); }
    }
}

/* Pide a cada SN lo que mis viewers necesitan por encima de la tasa base. */
static void send_subscriptions(smnet_node_t* n, double now) {
    sn_t* s = n->sn;
    const int per = (SMNET_MTU - SMNET_HDR_SIZE - 2) / 3;
    uint8_t buf[SMNET_MTU];
    for (int oi = 0; oi < s->nothers; oi++) {
        other_t* o = &s->others[oi];
        int part = 0, cnt = 0, cpos = 0;
        wbuf_t w = { buf, 0, sizeof buf, 0 };
        node_hdr(n, &w, MSG_SUBSCRIBE, 0, 0);
        cpos = w.len;
        w_u16(&w, 0);
        for (int e = 0; e < SMNET_MAX_PLAYERS; e++) {
            ent_t* ee = &s->ents[e];
            if (!s->want[e] || ee->hosted || ee->host_sn != o->id || !alive_ent(n, ee, now)) continue;
            if (cnt == per) {
                buf[cpos] = (uint8_t)cnt; buf[cpos + 1] = (uint8_t)(cnt >> 8);
                node_send_id(n, o->id, &o->addr, buf, w.len);
                part++;
                w.len = 0;
                node_hdr(n, &w, MSG_SUBSCRIBE, (uint8_t)(part & 0x0F), 0);
                cpos = w.len;
                w_u16(&w, 0);
                cnt = 0;
            }
            w_u16(&w, (uint16_t)e);
            w_u8(&w, s->want[e]);
            cnt++;
        }
        buf[cpos] = (uint8_t)cnt; buf[cpos + 1] = (uint8_t)(cnt >> 8);
        node_send_id(n, o->id, &o->addr, buf, w.len);   /* aunque esté vacía: renueva (y limpia) */
    }
}

static void send_report(smnet_node_t* n, double now) {
    sn_t* s = n->sn;
    uint8_t buf[SMNET_MTU];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    double dt = (now - s->window_start) / 1000.0;
    if (dt > 0.2) {
        s->egress_kbps = (uint32_t)(s->tx_bytes_window * 8 / dt / 1000);
        s->peer_egress_kbps = (uint32_t)(s->peer_bytes_window * 8 / dt / 1000);
        s->tx_bytes_window = 0;
        s->peer_bytes_window = 0;
        s->window_start = now;
    }
    node_hdr(n, &w, MSG_SN_REPORT, 0, 0);
    w_u32(&w, n->epoch_seen);
    w_u32(&w, s->egress_kbps);
    int cpos = w.len, cnt = 0;
    w_u16(&w, 0);
    for (int i = 0; i < SMNET_MAX_PLAYERS && w.len + 2 <= (int)sizeof buf; i++) {
        ent_t* e = &s->ents[i];
        if (!e->hosted || now - e->last_up_ms > n->cfg.sn.hosted_expire_ms) continue;
        w_u16(&w, (uint16_t)i);
        cnt++;
    }
    buf[cpos] = (uint8_t)cnt; buf[cpos + 1] = (uint8_t)(cnt >> 8);
    w_u32(&w, s->peer_egress_kbps);   /* v1.3: egreso hacia los peers alojados (para el cupo) */
    if (w.err) return;
    node_send_coord(n, buf, w.len);
}

void sn_update(smnet_node_t* n, double now) {
    sn_t* s = n->sn;
    const smnet_sn_cfg_t* c = &n->cfg.sn;
    if (now - s->last_report >= c->report_every_ms) {
        send_report(n, now);
        s->last_report = now;
        /* SN sin sesión todavía (perforación perdida): reintentar una vez por segundo */
        for (int i = 0; i < n->nsn; i++)
            if (n->snlist[i].id != n->id && !node_has_link(n, &n->snlist[i].addr)) node_punch(n, &n->snlist[i].addr);
    }
    if (now < s->next_tick) return;
    s->tick++;
    s->next_tick += TICK_MS;
    if (s->next_tick < now) s->next_tick = now + TICK_MS;

    /* bajas de alojados */
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
        ent_t* e = &s->ents[i];
        if (e->hosted && now - e->last_up_ms > c->hosted_expire_ms) { e->hosted = 0; s->dirty = 1; }
        if (!e->hosted && e->v && now - e->last_up_ms > 3 * c->hosted_expire_ms) { view_free(e->v); e->v = NULL; }
    }
    if (s->dirty || s->tick % c->rank_every_ticks == 0) { rerank(n, now); s->dirty = 0; }
    s->hosted_cached = 0;
    for (int v = 0; v < SMNET_MAX_PLAYERS; v++) {
        ent_t* ev = &s->ents[v];
        if (ev->hosted && now - ev->last_up_ms <= c->hosted_expire_ms) s->hosted_cached++;
    }
    for (int v = 0; v < SMNET_MAX_PLAYERS; v++) {
        ent_t* ev = &s->ents[v];
        if (ev->hosted && ev->v && now - ev->last_up_ms <= c->hosted_expire_ms) send_batches(n, (uint16_t)v, now);
    }
    send_relays(n, now);
    sn_forward_proofs(n);
    for (int e = 0; e < SMNET_MAX_PLAYERS; e++) s->ents[e].arrived = 0;
    if (s->sub_now || now - s->last_subscribe >= c->subscribe_every_ms) { send_subscriptions(n, now); s->last_subscribe = now; s->sub_now = 0; }
}
