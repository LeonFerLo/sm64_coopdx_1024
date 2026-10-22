/* Rol coordinador (§9, §10.3, §12): altas, elección de SN, asignación por costo mínimo,
 * backups, reemplazo de SN caídos, réplicas en caliente y fencing por época. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "node_internal.h"
#include "../third_party/monocypher/monocypher.h"
#include "assign.h"

static const smnet_addr_t NOADDR = { 0, { 0 } };

coord_t* coord_new(void) {
    coord_t* c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->nodes = calloc(SMNET_MAX_PLAYERS, sizeof(cnode_t));
    if (!c->nodes) { free(c); return NULL; }
    c->est_down_kbps = 300.f;
    return c;
}

void coord_free(coord_t* c) {
    if (!c) return;
    free(c->nodes);
    free(c);
}

/* ------------------------------------------------------------ utilidades */

static int alive_sn(const coord_t* c, uint16_t id) {
    return id < SMNET_MAX_PLAYERS && c->nodes[id].used && c->nodes[id].is_sn;
}

static float rtt_of(const smnet_node_t* n, const cnode_t* p, uint16_t sn) {
    for (int k = 0; k < p->n_rtt; k++) if (p->rtt_id[k] == sn) return (float)p->rtt_ms[k];
    return n->cfg.coord.unknown_rtt_ms;
}

static int nat_ok(const coord_t* c, const cnode_t* p, uint16_t sn) {
    return p->nat != SMNET_NAT_SYMMETRIC || c->nodes[sn].nat == SMNET_NAT_OPEN;
}

static float score(const smnet_node_t* n, const cnode_t* p, double now) {
    double up = log10(p->upload_kbps > 1000 ? p->upload_kbps / 1000.0 : 1.0) / 3.0;
    double sum = 0; int k = 0;
    for (; k < p->n_rtt; k++) sum += p->rtt_ms[k];
    double cen = k ? sum / k : 125.0;
    double cn = 1.0 - cen / 250.0; if (cn < 0) cn = 0; if (cn > 1) cn = 1;
    double upt = (now - p->join_ms) / 600000.0; if (upt > 1) upt = 1; if (upt < 0) upt = 0;
    (void)n;
    return (float)(0.5 * up + 0.3 * cn + 0.2 * upt);
}

static uint16_t capacity(const smnet_node_t* n, const coord_t* c, const cnode_t* p) {
    const smnet_coord_cfg_t* k = &n->cfg.coord;
    double per_peer = c->est_down_kbps * 1.5;
    double cap = floor(k->sn_usable * p->upload_kbps / per_peer) - 1;
    if (cap < 1) cap = 1;
    if (cap > k->max_fanout) cap = k->max_fanout;
    return (uint16_t)cap;
}

static void free_node(coord_t* c, uint16_t id, double now) {
    cnode_t* p = &c->nodes[id];
    p->used = 0;
    p->is_sn = 0;
    p->freed_ms = now;
}

static void write_ref(wbuf_t* w, const coord_t* c, uint16_t id) {
    if (id < SMNET_MAX_PLAYERS && c->nodes[id].used) { w_u16(w, id); addr_write(w, &c->nodes[id].addr); }
    else { w_u16(w, SMNET_NO_ID); addr_write(w, &NOADDR); }
}

static void send_to_node(smnet_node_t* n, uint16_t id, const uint8_t* buf, int len) {
    node_send_id(n, id, &n->co->nodes[id].addr, buf, len);
}

/* Si el SN tiene NAT (cono), tiene que abrir su filtro hacia el peer antes de que este llegue. */
static void intro_to_sn(smnet_node_t* n, uint16_t sn, uint16_t peer) {
    coord_t* c = n->co;
    if (sn >= SMNET_MAX_PLAYERS || peer >= SMNET_MAX_PLAYERS || sn == peer) return;
    cnode_t* s = &c->nodes[sn];
    if (!s->used || s->nat == SMNET_NAT_OPEN) return;
    node_send_intro(n, sn, &s->addr, peer, &c->nodes[peer].addr);
}

static void send_assign(smnet_node_t* n, uint16_t id) {
    coord_t* c = n->co;
    intro_to_sn(n, c->nodes[id].primary, id);
    intro_to_sn(n, c->nodes[id].backup, id);
    uint8_t buf[128];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    node_hdr(n, &w, MSG_ASSIGN, 0, 0);
    w_u32(&w, c->epoch);
    write_ref(&w, c, c->nodes[id].primary);
    write_ref(&w, c, c->nodes[id].backup);
    send_to_node(n, id, buf, w.len);
    c->nodes[id].dirty = 0;
    c->nodes[id].last_assign_ms = n->now;
}

static void send_epoch_msg(smnet_node_t* n, uint16_t id, uint8_t type, int with_cap) {
    coord_t* c = n->co;
    uint8_t buf[32];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    node_hdr(n, &w, type, 0, 0);
    w_u32(&w, c->epoch);
    if (with_cap) w_u16(&w, c->nodes[id].cap);
    send_to_node(n, id, buf, w.len);
}

static int build_snlist(smnet_node_t* n, uint8_t* buf, int cap) {
    coord_t* c = n->co;
    wbuf_t w = { buf, 0, cap, 0 };
    node_hdr(n, &w, MSG_SN_LIST, 0, 0);
    w_u32(&w, c->epoch);
    w_u16(&w, n->id);
    addr_write(&w, &n->cfg.self_addr);
    int cpos = w.len, cnt = 0;
    w_u8(&w, 0);
    for (int i = 0; i < SMNET_MAX_PLAYERS && cnt < SMNET_MAX_SN; i++) {
        cnode_t* p = &c->nodes[i];
        if (!p->used || !p->is_sn) continue;
        uint8_t f = p->nat == SMNET_NAT_OPEN ? SNF_OPEN : 0;
        for (int r = 0; r < c->nreplicas; r++) if (c->replicas[r] == i) f |= SNF_REPLICA;
        w_u16(&w, (uint16_t)i);
        addr_write(&w, &p->addr);
        w_u8(&w, f);
        cnt++;
    }
    buf[cpos] = (uint8_t)cnt;
    /* v1.4: los degradados (permanentes) vigentes: nadie acepta un NEW_COORD de ellos */
    int bpos = w.len, nb = 0;
    w_u8(&w, 0);
    for (int i = 0; i < SMNET_MAX_PLAYERS && nb < BANLIST_MAX; i++)
        if (c->nodes[i].used && c->nodes[i].banned_until > 1e299) { w_u16(&w, (uint16_t)i); nb++; }
    buf[bpos] = (uint8_t)nb;
    return w.err ? 0 : w.len;
}

static void broadcast_snlist(smnet_node_t* n, double now) {
    coord_t* c = n->co;
    uint8_t buf[SMNET_MTU];
    int len = build_snlist(n, buf, sizeof buf);
    if (!len) return;
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) if (c->nodes[i].used) send_to_node(n, (uint16_t)i, buf, len);
    c->snlist_dirty = 0;
    c->last_snlist_bcast = now;
}

/* SN para un peer cuando no se puede esperar al rebalanceo (entrada, failover, caídas). */
static uint16_t pick_sn(smnet_node_t* n, uint16_t pid, uint16_t exclude, int for_backup) {
    coord_t* c = n->co;
    const smnet_coord_cfg_t* k = &n->cfg.coord;
    cnode_t* p = &c->nodes[pid];
    static uint16_t load[SMNET_MAX_PLAYERS];
    memset(load, 0, sizeof load);
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
        cnode_t* q = &c->nodes[i];
        if (!q->used || q->is_sn) continue;
        uint16_t s = for_backup ? q->backup : q->primary;
        if (alive_sn(c, s)) load[s]++;
    }
    uint16_t best = SMNET_NO_ID;
    float bv = 1e30f;
    for (int s = 0; s < SMNET_MAX_PLAYERS; s++) {
        if (!alive_sn(c, (uint16_t)s) || s == exclude || s == pid || !nat_ok(c, p, (uint16_t)s)) continue;
        float capf = c->nodes[s].cap * k->emergency_overflow;
        if (load[s] >= capf) continue;
        float v = rtt_of(n, p, (uint16_t)s) + k->beta_ms * load[s] / (c->nodes[s].cap ? c->nodes[s].cap : 1);
        if (v < bv) { bv = v; best = (uint16_t)s; }
    }
    return best;
}

/* ------------------------------------------------------------ elección */

static void promote(smnet_node_t* n, uint16_t id, double now) {
    coord_t* c = n->co;
    cnode_t* p = &c->nodes[id];
    p->is_sn = 1;
    p->cap = capacity(n, c, p);
    p->sn_since_ms = now;
    p->last_report_ms = now;
    p->primary = id;
    p->backup = SMNET_NO_ID;
    p->dirty = 1;
    p->got_report = 0;
    p->promote_tries = 1;
    p->suspects = 0;
    c->snlist_dirty = 1;
    n->stats.co_promotions++;
    send_epoch_msg(n, id, MSG_PROMOTE, 1);
}

static int elect(smnet_node_t* n, double now) {
    coord_t* c = n->co;
    const smnet_coord_cfg_t* k = &n->cfg.coord;
    int peers = 0, sym = 0, nsn = 0;
    double cap = 0, open_cap = 0;
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
        cnode_t* p = &c->nodes[i];
        if (!p->used) continue;
        if (p->is_sn) {
            p->cap = capacity(n, c, p);
            cap += p->cap; nsn++;
            if (p->nat == SMNET_NAT_OPEN) open_cap += p->cap;
        } else {
            peers++;
            if (p->nat == SMNET_NAT_SYMMETRIC) sym++;
        }
    }
    int changed = 0;
    for (int pass = 0; pass < 2; pass++) {
        /* pass 0: capacidad total; pass 1: capacidad con NAT abierto para los simétricos */
        for (;;) {
            double need = pass == 0 ? k->margin * peers : k->margin * sym;
            double have = pass == 0 ? cap : open_cap;
            if (have >= need) break;
            int urgent = (pass == 0 ? cap : open_cap) < (pass == 0 ? peers : sym);
            int best = -1; float bs = -1;
            for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
                cnode_t* p = &c->nodes[i];
                if (!p->used || p->is_sn || p->nat == SMNET_NAT_SYMMETRIC) continue;
                if (p->banned_until > now) continue;   /* degradado por mal comportamiento */
                if (pass == 1 && p->nat != SMNET_NAT_OPEN) continue;
                if (p->upload_kbps < k->sn_min_upload_kbps && !(urgent && nsn == 0)) continue;
                if (now - p->join_ms < k->sn_min_uptime_ms && !urgent) continue;
                float sc = score(n, p, now);
                if (sc > bs) { bs = sc; best = i; }
            }
            if (best < 0) break;
            promote(n, (uint16_t)best, now);
            cnode_t* p = &c->nodes[best];
            cap += p->cap; nsn++; peers--;
            if (p->nat == SMNET_NAT_OPEN) open_cap += p->cap;
            changed = 1;
        }
    }
    /* sobra capacidad: se degrada uno por ciclo (nunca el coordinador ni una réplica) */
    if (cap > k->demote_margin * peers && nsn > 1) {
        int worst = -1; float ws = 1e9f;
        for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
            cnode_t* p = &c->nodes[i];
            if (!p->used || !p->is_sn || i == n->id) continue;
            int rep = 0;
            for (int r = 0; r < c->nreplicas; r++) if (c->replicas[r] == i) rep = 1;
            if (rep) continue;
            if (cap - p->cap < k->margin * (peers + 1)) continue;
            if (p->nat == SMNET_NAT_OPEN && open_cap - p->cap < k->margin * sym) continue;
            float sc = score(n, p, now);
            if (sc < ws) { ws = sc; worst = i; }
        }
        if (worst >= 0 && c->npend < 64) {
            cnode_t* p = &c->nodes[worst];
            p->is_sn = 0;
            p->primary = SMNET_NO_ID;
            p->backup = SMNET_NO_ID;
            c->pend[c->npend].id = (uint16_t)worst;
            c->pend[c->npend].at = now + 2000;   /* drena: sus peers se mudan antes */
            c->pend[c->npend].epoch = c->epoch;
            c->npend++;
            c->snlist_dirty = 1;
            n->stats.co_demotions++;
            changed = 1;
        }
    }
    return changed;
}

/* ------------------------------------------------------------ caídas */

static void sn_dead(smnet_node_t* n, uint16_t id, double now) {
    coord_t* c = n->co;
    n->stats.co_sn_deaths++;
    free_node(c, id, now);
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
        cnode_t* p = &c->nodes[i];
        if (!p->used) continue;
        if (p->primary == id) {
            /* el peer hace failover solo a su backup; acá solo se refleja */
            p->primary = alive_sn(c, p->backup) ? p->backup : SMNET_NO_ID;
            p->backup = SMNET_NO_ID;
            p->dirty = 1;
        } else if (p->backup == id) {
            p->backup = SMNET_NO_ID;
            p->dirty = 1;
        }
    }
    for (int r = 0; r < c->nreplicas; r++) if (c->replicas[r] == id) { c->replicas[r] = c->replicas[--c->nreplicas]; r--; }
    c->snlist_dirty = 1;
}

static void liveness(smnet_node_t* n, double now) {
    coord_t* c = n->co;
    const smnet_coord_cfg_t* k = &n->cfg.coord;
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
        cnode_t* p = &c->nodes[i];
        if (!p->used || i == n->id) continue;
        if (p->is_sn) {
            if (!p->got_report && p->promote_tries < 4 && now - p->sn_since_ms > 700.0 * p->promote_tries) {
                /* control por UDP sin confirmación: si no reportó todavía, el PROMOTE pudo perderse */
                send_epoch_msg(n, (uint16_t)i, MSG_PROMOTE, 1);
                p->promote_tries++;
            }
            if (now - p->last_report_ms > k->sn_dead_ms) sn_dead(n, (uint16_t)i, now);
        } else {
            double last = p->last_seen_ms > p->last_hosted_ms ? p->last_seen_ms : p->last_hosted_ms;
            if (now - last > k->peer_dead_ms) free_node(c, (uint16_t)i, now);
        }
    }
}

/* Completa primarios/backups faltantes sin esperar al rebalanceo. */
static void fill_missing(smnet_node_t* n) {
    coord_t* c = n->co;
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
        cnode_t* p = &c->nodes[i];
        if (!p->used) continue;
        if (p->is_sn) {
            if (!alive_sn(c, p->backup) || p->backup == i) {
                uint16_t b = pick_sn(n, (uint16_t)i, (uint16_t)i, 1);
                if (b != p->backup) { p->backup = b; p->dirty = 1; }
            }
            continue;
        }
        if (!alive_sn(c, p->primary)) {
            uint16_t s = pick_sn(n, (uint16_t)i, SMNET_NO_ID, 0);
            if (s != SMNET_NO_ID) { p->primary = s; p->dirty = 1; }
        }
        if (alive_sn(c, p->primary) && (!alive_sn(c, p->backup) || p->backup == p->primary)) {
            uint16_t b = pick_sn(n, (uint16_t)i, p->primary, 1);
            if (b != p->backup) { p->backup = b; p->dirty = 1; }
        }
    }
}

/* ------------------------------------------------------------ rebalanceo */

static void send_replica_hb(smnet_node_t* n, uint16_t to, uint8_t rank) {
    uint8_t buf[32];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    node_hdr(n, &w, MSG_COORD_HB, 0, 0);
    w_u32(&w, n->co->epoch);
    w_u8(&w, rank);   /* 0xFF = ya no sos réplica */
    send_to_node(n, to, buf, w.len);
}

/* Réplicas estables: se conservan mientras sigan siendo SN vivos; solo se completan los
 * lugares vacíos con los mejores por score. A una réplica descartada se le avisa (rango 0xFF)
 * para que no confunda el silencio con la caída del coordinador. */
static void choose_replicas(smnet_node_t* n, double now) {
    coord_t* c = n->co;
    int want = n->cfg.coord.replicas;
    if (want > 4) want = 4;
    uint16_t best[4];
    int nb = 0;
    for (int r = 0; r < c->nreplicas && nb < want; r++) {
        uint16_t id = c->replicas[r];
        if (alive_sn(c, id) && id != n->id) best[nb++] = id;
        else if (id < SMNET_MAX_PLAYERS && c->nodes[id].used) send_replica_hb(n, id, 0xFF);
    }
    while (nb < want) {
        int bi = -1; float bs = -1;
        for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
            cnode_t* p = &c->nodes[i];
            if (!p->used || !p->is_sn || i == n->id) continue;
            int taken = 0;
            for (int j = 0; j < nb; j++) if (best[j] == i) taken = 1;
            if (taken) continue;
            if (p->banned_until > now) continue;
            /* con NAT abierto primero: si toma el control, todos tienen que poder llegarle */
            float sc = score(n, p, now) + (p->nat == SMNET_NAT_OPEN ? 10.f : 0.f);
            if (sc > bs) { bs = sc; bi = i; }
        }
        if (bi < 0) break;
        best[nb++] = (uint16_t)bi;
    }
    if (nb != c->nreplicas || memcmp(best, c->replicas, sizeof(uint16_t) * (size_t)nb) != 0) {
        memcpy(c->replicas, best, sizeof(uint16_t) * (size_t)nb);
        c->nreplicas = nb;
        c->snlist_dirty = 1;
        c->last_sync = 0;   /* sincronizar ya a las nuevas */
    }
}

static void rebalance(smnet_node_t* n, double now) {
    coord_t* c = n->co;
    const smnet_coord_cfg_t* k = &n->cfg.coord;
    n->stats.co_rebalances++;
    liveness(n, now);
    elect(n, now);

    uint16_t sns[SMNET_MAX_SN];
    int m = 0;
    static uint16_t peers[SMNET_MAX_PLAYERS];
    int np = 0;
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
        cnode_t* p = &c->nodes[i];
        if (!p->used) continue;
        if (p->is_sn) { if (m < SMNET_MAX_SN) sns[m++] = (uint16_t)i; }
        else peers[np++] = (uint16_t)i;
    }
    if (m > 0 && np > 0) {
        float* cost = malloc(sizeof(float) * (size_t)(np * m));
        float* base = malloc(sizeof(float) * (size_t)(np * m));
        int* capv = malloc(sizeof(int) * (size_t)m);
        int* out = malloc(sizeof(int) * (size_t)np);
        int* oldcol = malloc(sizeof(int) * (size_t)np);
        if (cost && base && capv && out && oldcol) {
            for (int s = 0; s < m; s++) capv[s] = c->nodes[sns[s]].cap;
            for (int a = 0; a < np; a++) {
                cnode_t* p = &c->nodes[peers[a]];
                oldcol[a] = -1;
                for (int s = 0; s < m; s++) {
                    float v = nat_ok(c, p, sns[s]) ? rtt_of(n, p, sns[s]) : SMNET_ASSIGN_INF;
                    base[a * m + s] = v;
                    if (p->primary == sns[s]) { oldcol[a] = s; if (v < SMNET_ASSIGN_INF) v -= k->hysteresis_ms; }
                    cost[a * m + s] = v;
                }
            }
            smnet_assign_solve(np, m, cost, capv, k->beta_ms, out);
            /* tope de movimientos por ciclo: se revierten los de menor ganancia */
            int moved = 0, max_moves = (int)(k->max_move_frac * np);
            if (max_moves < 1) max_moves = 1;
            /* los asignados "a ciegas" al entrar (sin RTT medido) se ubican sin tope */
            for (int a = 0; a < np; a++) {
                cnode_t* p = &c->nodes[peers[a]];
                if (p->provisional && p->n_rtt > 0) oldcol[a] = -1 - 1;   /* -2: libre de moverse */
            }
            for (int a = 0; a < np; a++) if (oldcol[a] >= 0 && out[a] >= 0 && out[a] != oldcol[a]) moved++;
            if (moved > max_moves) {
                int* load = calloc((size_t)m, sizeof(int));
                for (int a = 0; a < np; a++) if (out[a] >= 0) load[out[a]]++;
                for (int left = moved - max_moves; left > 0; left--) {
                    int wa = -1; float wg = 1e30f;
                    for (int a = 0; a < np; a++) {
                        if (oldcol[a] < 0 || out[a] < 0 || out[a] == oldcol[a] || load[oldcol[a]] >= capv[oldcol[a]]) continue;
                        float g = base[a * m + oldcol[a]] - base[a * m + out[a]];
                        if (g < wg) { wg = g; wa = a; }
                    }
                    if (wa < 0) break;
                    load[out[wa]]--; load[oldcol[wa]]++;
                    out[wa] = oldcol[wa];
                }
                free(load);
            }
            /* aplicar primarios */
            for (int a = 0; a < np; a++) {
                cnode_t* p = &c->nodes[peers[a]];
                if (oldcol[a] == -2) p->provisional = 0;
                uint16_t np_ = out[a] >= 0 ? sns[out[a]] : (alive_sn(c, p->primary) ? p->primary : SMNET_NO_ID);
                if (np_ != p->primary) {
                    if (oldcol[a] >= 0) n->stats.co_moves++;
                    p->primary = np_;
                    p->dirty = 1;
                }
            }
            /* backups: el mejor SN distinto del primario, con lugar de backup */
            int* bload = calloc((size_t)m, sizeof(int));
            for (int a = 0; a < np; a++) {
                cnode_t* p = &c->nodes[peers[a]];
                int bs = -1; float bv = 1e30f;
                for (int s = 0; s < m; s++) {
                    if (sns[s] == p->primary || base[a * m + s] >= SMNET_ASSIGN_INF || bload[s] >= capv[s]) continue;
                    float v = base[a * m + s] + (sns[s] == p->backup ? -k->hysteresis_ms : 0);
                    if (v < bv) { bv = v; bs = s; }
                }
                uint16_t nb = bs >= 0 ? sns[bs] : SMNET_NO_ID;
                if (bs >= 0) bload[bs]++;
                if (nb != p->backup) { p->backup = nb; p->dirty = 1; }
            }
            free(bload);
        }
        free(cost); free(base); free(capv); free(out); free(oldcol);
    }
    /* los SN: primario = ellos mismos, backup = otro SN */
    for (int s = 0; s < m; s++) {
        cnode_t* p = &c->nodes[sns[s]];
        if (p->primary != sns[s]) { p->primary = sns[s]; p->dirty = 1; }
        if (!alive_sn(c, p->backup) || p->backup == sns[s]) {
            uint16_t b = pick_sn(n, sns[s], sns[s], 1);
            if (b != p->backup) { p->backup = b; p->dirty = 1; }
        }
    }
    choose_replicas(n, now);
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) if (c->nodes[i].used && c->nodes[i].dirty) send_assign(n, (uint16_t)i);
    broadcast_snlist(n, now);
    c->last_rebalance = now;
}

/* ------------------------------------------------------- degradación */

/* Saca a un SN de su rol: sus peers van a otros SN ya, deja de ser elegible (10 min o para
 * siempre) y sale de la lista de SN (los demás SN dejan de aceptarle reenvíos). */
static void ban_node(smnet_node_t* n, uint16_t sid, double now, int permanent) {
    coord_t* c = n->co;
    cnode_t* b = &c->nodes[sid];
    b->banned_until = permanent ? 1e300 : now + 600000.0;
    if (permanent) n->stats.co_bans++; else n->stats.co_bans_temp++;
    n->stats.co_last_ban_ms = now;
    n->stats.co_last_banned = sid;
    if (!b->is_sn) return;
    b->is_sn = 0;
    send_epoch_msg(n, sid, MSG_DEMOTE, 0);
    for (int r = 0; r < c->nreplicas; r++) if (c->replicas[r] == sid) {
        send_replica_hb(n, sid, 0xFF);   /* baja explícita: que no confunda el silencio con una caída */
        c->replicas[r] = c->replicas[--c->nreplicas]; r--;
    }
    if (permanent) { node_ban_local(n, sid); }
    elect(n, now);
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
        cnode_t* p = &c->nodes[i];
        if (!p->used || i == sid) continue;
        if (p->primary == sid) { p->primary = pick_sn(n, (uint16_t)i, sid, 0); p->dirty = 1; }
        if (p->backup == sid || p->backup == p->primary) { p->backup = pick_sn(n, (uint16_t)i, p->primary, 1); p->dirty = 1; }
        if (p->dirty) send_assign(n, (uint16_t)i);
    }
    choose_replicas(n, now);
    broadcast_snlist(n, now);
}

/* GAP_REPORT de un viewer. Se guardan los últimos 10 s de reportes y se decide:
 *   - si los reportes sobre entidades que viven en un mismo SN X llegan por 2 o más SN de entrega
 *     distintos (con 3 testigos distintos), el culpable es X: altera o descarta lo que reenvía;
 *   - si no, si 3 testigos distintos atendidos por el mismo SN D reportan, el culpable es D;
 *   - si D es casi vacío (la mitad de sus alojados reporta), degradación temporal de 10 min.
 * El SN de entrega es el primario que tiene registrado el coordinador, no lo que diga el reporte. */
#define BLAME_WAIT_MS 6000.0
/* ¿Los reportes "por dsn, de cosas de h" ya alcanzan para culpar a dsn? Si h también aparece por
 * otro SN, el problema es de h (lo resuelve la regla del origen); si no, se espera BLAME_WAIT_MS. */
static int blame_dsn_for_host(const coord_t* c, uint16_t dsn, uint16_t h, double now) {
    double first = now;
    for (int i = 0; i < c->nrep; i++) {
        if (c->rep[i].host != h) continue;
        if (c->rep[i].dsn != dsn) return 0;
        if (c->rep[i].t < first) first = c->rep[i].t;
    }
    return now - first >= BLAME_WAIT_MS;
}

static void on_gap_report(smnet_node_t* n, uint16_t reporter, rbuf_t* r, double now) {
    coord_t* c = n->co;
    uint8_t reason = r_u8(r);
    uint16_t said = r_u16(r);
    int cnt = r_u8(r);
    uint16_t ids[VER_TRACK];
    if (cnt > VER_TRACK) cnt = VER_TRACK;
    for (int i = 0; i < cnt; i++) ids[i] = r_u16(r);
    if (r->err || reason < GAP_DROP || reason > GAP_NOPROOF) return;
    n->stats.co_reports++;
    uint16_t dsn = c->nodes[reporter].primary;
    if (!alive_sn(c, dsn)) return;
    /* reporte viejo (se lo movió hace poco, o habla de otro SN): no cuenta */
    if (said != dsn || now - c->nodes[reporter].last_assign_ms < 3000) return;
    /* entidades reasignadas hace poco (por ejemplo, porque se acaba de degradar a su SN): lo que
       vio el viewer es de la asignación anterior y no dice nada de la actual */
    int k = 0;
    for (int i = 0; i < cnt; i++) {
        if (ids[i] >= SMNET_MAX_PLAYERS || !c->nodes[ids[i]].used) return;
        if (now - c->nodes[ids[i]].last_assign_ms >= 3000) ids[k++] = ids[i];
    }
    if (k == 0 || (reason != GAP_ALTERED && k < (cnt + 1) / 2)) return;
    cnt = k;
    uint16_t host = SMNET_NO_ID;
    for (int i = 0; i < cnt; i++) {
        uint16_t h = c->nodes[ids[i]].is_sn ? ids[i] : c->nodes[ids[i]].primary;
        if (i == 0) host = h;
        else if (h != host) { host = SMNET_NO_ID; break; }
    }
    /* anotar */
    int w = 0;
    for (int i = 0; i < c->nrep; i++) if (now - c->rep[i].t <= 10000) c->rep[w++] = c->rep[i];
    c->nrep = w;
    if (c->nrep == (int)(sizeof c->rep / sizeof c->rep[0])) { memmove(c->rep, c->rep + 1, sizeof c->rep[0] * (size_t)(c->nrep - 1)); c->nrep--; }
    c->rep[c->nrep].t = now; c->rep[c->nrep].reporter = reporter; c->rep[c->nrep].dsn = dsn; c->rep[c->nrep].host = host;
    c->nrep++;
    /* ¿el SN de origen? */
    if (host != SMNET_NO_ID && host != dsn && alive_sn(c, host) && !(c->nodes[host].banned_until > now)) {
        uint16_t rs[64], ds[64]; int nr = 0, nd = 0;
        for (int i = 0; i < c->nrep; i++) {
            if (c->rep[i].host != host) continue;
            int seen = 0;
            for (int k = 0; k < nr; k++) if (rs[k] == c->rep[i].reporter) seen = 1;
            if (!seen && nr < 64) rs[nr++] = c->rep[i].reporter;
            seen = 0;
            for (int k = 0; k < nd; k++) if (ds[k] == c->rep[i].dsn) seen = 1;
            if (!seen && nd < 64) ds[nd++] = c->rep[i].dsn;
        }
        if (nd >= 2 && nr >= 3 && host != n->id) { ban_node(n, host, now, 1); return; }
    }
    /* ¿el SN que entrega? */
    if (c->nodes[dsn].banned_until > now || dsn == n->id) return;   /* el coordinador no se degrada a sí mismo */
    uint16_t rs[64]; int nr = 0;
    for (int i = 0; i < c->nrep; i++) {
        if (c->rep[i].dsn != dsn) continue;
        /* un reporte que señala a un único SN de origen (otro) apunta a ese origen: cuenta contra el
           que entrega solo si lleva BLAME_WAIT_MS sin que nadie más reporte a ese origen */
        uint16_t h = c->rep[i].host;
        if (h != SMNET_NO_ID && h != dsn && !blame_dsn_for_host(c, dsn, h, now)) continue;
        int seen = 0;
        for (int k = 0; k < nr; k++) if (rs[k] == c->rep[i].reporter) seen = 1;
        if (!seen && nr < 64) rs[nr++] = c->rep[i].reporter;
    }
    int hosted = 0;
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) if (c->nodes[i].used && !c->nodes[i].is_sn && c->nodes[i].primary == dsn) hosted++;
    if (nr >= 3) ban_node(n, dsn, now, 1);
    else if (nr >= 2 && nr * 2 >= hosted) ban_node(n, dsn, now, 0);   /* SN casi vacío (al menos 2 testigos) */
}

/* ------------------------------------------------------------ altas */

/* ------------------------------------------------------------ moderación */

static int coord_is_blocked(coord_t* c, const uint8_t* pk, double now) {
    for (int i = 0; i < c->nblocked; i++) {
        if (c->blocked[i].until <= now) { c->blocked[i] = c->blocked[--c->nblocked]; i--; continue; }
        if (memcmp(c->blocked[i].pk, pk, 32) == 0) return 1;
    }
    return 0;
}
static int coord_is_member(const coord_t* c, const uint8_t* pk) {
    for (int i = 0; i < c->nmembers; i++) if (memcmp(c->members[i], pk, 8) == 0) return 1;
    return 0;
}
static void coord_add_member(coord_t* c, const uint8_t* pk) {
    if (coord_is_member(c, pk)) return;
    if (c->nmembers < 2048) { memcpy(c->members[c->nmembers++], pk, 8); return; }
    memcpy(c->members[c->members_pos], pk, 8);
    c->members_pos = (c->members_pos + 1) % 2048;
}
static int coord_active(const smnet_node_t* n) { return n->co && n->co->role == SMNET_COORD_ACTIVE; }

void smnet_node_identity(const smnet_node_t* n, uint8_t out[32]) { memcpy(out, link_identity(n->link), 32); }
int smnet_node_coord_identity(const smnet_node_t* n, uint16_t id, uint8_t out[32]) {
    if (!coord_active(n) || id >= SMNET_MAX_PLAYERS || !n->co->nodes[id].used) return -1;
    memcpy(out, n->co->nodes[id].pk, 32);
    return 0;
}
int smnet_node_coord_block(smnet_node_t* n, const uint8_t pk[32], double block_ms, double now) {
    if (!coord_active(n)) return -1;
    coord_t* c = n->co;
    double until = block_ms < 0 ? 1e300 : now + block_ms;
    for (int i = 0; i < c->nblocked; i++) if (memcmp(c->blocked[i].pk, pk, 32) == 0) {
        if (until > c->blocked[i].until) c->blocked[i].until = until;
        return 0;
    }
    int k = c->nblocked < 128 ? c->nblocked++ : 0;   /* lleno: se pisa el más viejo */
    memcpy(c->blocked[k].pk, pk, 32);
    c->blocked[k].until = until;
    return 0;
}
int smnet_node_coord_unblock_all(smnet_node_t* n) {
    if (!coord_active(n)) return -1;
    n->co->nblocked = 0;
    return 0;
}
int smnet_node_coord_set_closed(smnet_node_t* n, int closed) {
    if (!coord_active(n)) return -1;
    coord_t* c = n->co;
    if (closed && !c->closed)   /* los que están ahora pueden volver */
        for (int i = 0; i < SMNET_MAX_PLAYERS; i++) if (c->nodes[i].used) coord_add_member(c, c->nodes[i].pk);
    c->closed = closed ? 1 : 0;
    return 0;
}
int smnet_node_coord_expel(smnet_node_t* n, uint16_t id, double block_ms, double now) {
    if (!coord_active(n) || id >= SMNET_MAX_PLAYERS || id == n->id) return -1;
    coord_t* c = n->co;
    cnode_t* p = &c->nodes[id];
    if (!p->used) return -1;
    if (block_ms != 0) smnet_node_coord_block(n, p->pk, block_ms, now);
    int was_sn = p->is_sn;
    if (was_sn) sn_dead(n, id, now); else free_node(c, id, now);
    /* que el id no se le dé a otro mientras el echado todavía pueda andar con él */
    double hold = block_ms < 0 ? 1e12 : block_ms > 600000.0 ? block_ms : 600000.0;
    p->freed_ms = now + hold;
    if (was_sn) { elect(n, now); fill_missing(n); }
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) if (c->nodes[i].used && c->nodes[i].dirty) send_assign(n, (uint16_t)i);
    if (was_sn) broadcast_snlist(n, now);
    return 0;
}

static void on_hello(smnet_node_t* n, const smnet_addr_t* from, rbuf_t* r, double now) {
    coord_t* c = n->co;
    uint8_t vmin = r_u8(r), vmax = r_u8(r);
    uint32_t nonce = r_u32(r);
    uint32_t up = r_u32(r);
    uint8_t nat = r_u8(r);
    if (r->err || vmin > SMNET_PROTO_VERSION || vmax < SMNET_PROTO_VERSION || nat > 2) return;
    uint16_t want = r_u16(r);
    if (r->err) { want = SMNET_NO_ID; r->err = 0; }
    /* identidad del que entra: la de su sesión (la propia si es el host hablándose a sí mismo) */
    const uint8_t* ipk = smnet_addr_eq(from, &n->cfg.self_addr) ? link_identity(n->link) : link_peer_identity(n->link, from);
    if (!ipk) return;
    if (coord_is_blocked(c, ipk, now)) return;   /* echado o baneado: no se le contesta */
    int id = -1;
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++)
        if (c->nodes[i].used && c->nodes[i].nonce == nonce) { id = i; break; }
    if (id >= 0 && memcmp(c->nodes[id].pk, ipk, 32) != 0) {
        /* el nonce es de otro: quien lo repite con otra identidad no se queda con su lugar */
        n->stats.hello_hijack++;
        return;
    }
    if (id < 0 && c->closed && !coord_is_member(c, ipk)) return;   /* cerrada: solo los que ya estaban */
    if (id < 0) {
        /* el id pedido si está libre; si no, el primero libre */
        if (want < SMNET_MAX_PLAYERS && !c->nodes[want].used && (c->nodes[want].freed_ms == 0 || now - c->nodes[want].freed_ms > 10000)) id = want;
        for (int i = 0; i < SMNET_MAX_PLAYERS && id < 0; i++) {
            cnode_t* p = &c->nodes[i];
            if (!p->used && (p->freed_ms == 0 || now - p->freed_ms > 10000)) { id = i; break; }
        }
        if (id < 0) return;   /* lleno */
        cnode_t* p = &c->nodes[id];
        memset(p, 0, sizeof *p);
        p->used = 1;
        p->nonce = nonce;
        p->join_ms = now;
        p->primary = p->backup = SMNET_NO_ID;
        p->provisional = 1;
        memcpy(p->pk, ipk, 32);
        coord_add_member(c, ipk);
    }
    cnode_t* p = &c->nodes[id];
    p->addr = *from;
    p->upload_kbps = up;
    p->nat = nat;
    p->last_seen_ms = now;
    if (elect(n, now)) c->snlist_dirty = 1;
    if (!p->is_sn) {
        if (!alive_sn(c, p->primary)) p->primary = pick_sn(n, (uint16_t)id, SMNET_NO_ID, 0);
        if (!alive_sn(c, p->backup) || p->backup == p->primary) p->backup = pick_sn(n, (uint16_t)id, p->primary, 1);
    } else if (!alive_sn(c, p->backup) || p->backup == id) {
        p->backup = pick_sn(n, (uint16_t)id, (uint16_t)id, 1);
    }
    uint8_t buf[384];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    node_hdr(n, &w, MSG_WELCOME, 0, 0);
    w_u32(&w, nonce);
    w_u16(&w, (uint16_t)id);
    w_u32(&w, c->epoch);
    write_ref(&w, c, p->primary);
    write_ref(&w, c, p->backup);
    addr_write(&w, from);          /* cómo lo vemos (dirección pública) */
    w_bytes(&w, n->game_pk, 32);   /* v1.4: clave de la partida y certificado del que entra */
    uint8_t cert[CERT_SIZE];
    auth_make_cert(n, (uint16_t)id, p->pk, cert);
    w_bytes(&w, cert, CERT_SIZE);
    intro_to_sn(n, p->primary, (uint16_t)id);
    intro_to_sn(n, p->backup, (uint16_t)id);
    node_send_id(n, (uint16_t)id, from, buf, w.len);
    p->dirty = 0;
    /* la lista de SN de inmediato, para que mida RTT y conozca al coordinador */
    uint8_t lb[SMNET_MTU];
    int ll = build_snlist(n, lb, sizeof lb);
    if (ll) node_send_id(n, (uint16_t)id, from, lb, ll);
    if (c->snlist_dirty) {
        /* hubo promoción: los peers que ya estaban sin SN reciben uno */
        fill_missing(n);
        for (int i = 0; i < SMNET_MAX_PLAYERS; i++) if (c->nodes[i].used && c->nodes[i].dirty && i != id) send_assign(n, (uint16_t)i);
    }
}

/* ------------------------------------------------------------ réplicas */

static void send_sync(smnet_node_t* n, uint16_t to, int rank) {
    coord_t* c = n->co;
    uint8_t buf[SMNET_MTU];
    int total = 0;
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) total += c->nodes[i].used;
    const int per = (SMNET_MTU - SMNET_HDR_SIZE - 8 - 64) / 81;
    int nparts = (total + per - 1) / per;
    if (nparts == 0) nparts = 1;
    int part = 0, i = 0;
    while (part < nparts) {
        wbuf_t w = { buf, 0, sizeof buf, 0 };
        node_hdr(n, &w, MSG_COORD_SYNC, 0, 0);
        w_u32(&w, c->epoch);
        w_u8(&w, (uint8_t)rank);
        w_u8(&w, (uint8_t)part);
        w_u8(&w, (uint8_t)nparts);
        /* la clave secreta de la partida, para seguir emitiendo certificados si toma el control
           (viaja cifrada por el enlace) */
        w_bytes(&w, n->game_sk, 64);
        int cpos = w.len, cnt = 0;
        w_u8(&w, 0);
        for (; i < SMNET_MAX_PLAYERS && cnt < per; i++) {
            cnode_t* p = &c->nodes[i];
            if (!p->used) continue;
            w_u16(&w, (uint16_t)i);
            w_u32(&w, p->nonce);
            addr_write(&w, &p->addr);
            w_u32(&w, p->upload_kbps);
            w_u8(&w, p->nat);
            w_u8(&w, p->is_sn ? 1 : 0);
            w_u16(&w, p->primary);
            w_u16(&w, p->backup);
            w_u16(&w, p->area);
            w_u16(&w, p->cap);
            w_bytes(&w, p->pk, 32);
            cnt++;
        }
        buf[cpos] = (uint8_t)cnt;
        if (!w.err) send_to_node(n, to, buf, w.len);
        part++;
    }
}

static void takeover(smnet_node_t* n, double now) {
    coord_t* c = n->co;
    uint16_t old = c->coord_id;
    c->role = SMNET_COORD_ACTIVE;
    c->epoch += 1;
    n->stats.co_takeovers++;
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
        cnode_t* p = &c->nodes[i];
        if (!p->used) continue;
        p->last_seen_ms = p->last_hosted_ms = p->last_report_ms = now;   /* período de gracia */
        p->n_rtt = 0;
    }
    if (old < SMNET_MAX_PLAYERS && c->nodes[old].used) {
        if (c->nodes[old].is_sn) sn_dead(n, old, now);
        else free_node(c, old, now);
    }
    c->nreplicas = 0;
    c->npend = 0;
    c->took_over_ms = now;
    c->last_newcoord_resend = now;
    c->next_rebalance = now + n->cfg.coord.first_rebalance_ms;
    node_set_coord(n, n->id, &n->cfg.self_addr, c->epoch);
    uint8_t buf[160];
    int bl = auth_build_new_coord(n, c->epoch, buf, sizeof buf);
    for (int i = 0; i < SMNET_MAX_PLAYERS && bl; i++) if (c->nodes[i].used && i != n->id) send_to_node(n, (uint16_t)i, buf, bl);
    choose_replicas(n, now);
    fill_missing(n);
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) if (c->nodes[i].used && c->nodes[i].dirty) send_assign(n, (uint16_t)i);
    broadcast_snlist(n, now);
}

static void step_down(smnet_node_t* n) {
    n->co->role = SMNET_COORD_NONE;
}

/* ------------------------------------------------------------ API interna */

void coord_start_active(smnet_node_t* n, double now) {
    if (!n->co) n->co = coord_new();
    if (!n->co) return;
    n->co->role = SMNET_COORD_ACTIVE;
    n->co->epoch = 1;
    if (!n->have_game_sk) auth_init_game(n);
    n->co->next_rebalance = now + n->cfg.coord.rebalance_ms;
    n->epoch_seen = 1;
}

int coord_receive(smnet_node_t* n, const smnet_addr_t* from, const smnet_hdr_t* h, rbuf_t* r, double now) {
    coord_t* c = n->co;
    uint16_t id = h->sender;
    if (c->role == SMNET_COORD_REPLICA || c->role == SMNET_COORD_NONE) {
        if (h->type == MSG_NEW_COORD) {
            uint32_t epoch = r_u32(r);
            if (!r->err && epoch > c->epoch) { c->role = SMNET_COORD_NONE; c->epoch = epoch; c->last_hb_rx = 0; }
            return 1;
        }
        if (h->type == MSG_COORD_HB || h->type == MSG_COORD_SYNC) {
            uint32_t epoch = r_u32(r);
            uint8_t rank = r_u8(r);
            if (r->err || epoch < c->epoch || epoch < n->epoch_seen) return 0;
            if (rank == 0xFF) {   /* el coordinador nos dio de baja como réplica */
                c->role = SMNET_COORD_NONE;
                c->last_hb_rx = 0;
                return 1;
            }
            c->role = SMNET_COORD_REPLICA;
            c->epoch = epoch;
            c->my_rank = rank;
            c->coord_id = id;
            c->coord_addr = *from;
            c->last_hb_rx = now;
            c->probe_at = 0;
            node_set_coord(n, id, from, epoch);
            if (h->type == MSG_COORD_SYNC) {
                uint8_t part = r_u8(r), nparts = r_u8(r);
                uint8_t gsk[64];
                r_bytes(r, gsk, 64);
                if (!r->err && n->have_game) { memcpy(n->game_sk, gsk, 64); n->have_game_sk = 1; }
                crypto_wipe(gsk, 64);
                if (part == 0) c->sync_gen++;
                int cnt = r_u8(r);
                for (int k = 0; k < cnt && !r->err; k++) {
                    uint16_t nid = r_u16(r);
                    cnode_t tmp;
                    memset(&tmp, 0, sizeof tmp);
                    tmp.nonce = r_u32(r);
                    addr_read(r, &tmp.addr);
                    tmp.upload_kbps = r_u32(r);
                    tmp.nat = r_u8(r);
                    tmp.is_sn = r_u8(r) & 1;
                    tmp.primary = r_u16(r);
                    tmp.backup = r_u16(r);
                    tmp.area = r_u16(r);
                    tmp.cap = r_u16(r);
                    r_bytes(r, tmp.pk, 32);
                    if (r->err || nid >= SMNET_MAX_PLAYERS) break;
                    cnode_t* p = &c->nodes[nid];
                    double join = p->used && p->nonce == tmp.nonce ? p->join_ms : now;
                    *p = tmp;
                    p->used = 1;
                    p->join_ms = join;
                    p->sync_gen = c->sync_gen;
                }
                /* última parte: lo que faltó en dos sincronizaciones seguidas ya no existe */
                if (!r->err && part + 1 == nparts) {
                    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
                        cnode_t* p = &c->nodes[i];
                        if (p->used && (uint16_t)(c->sync_gen - p->sync_gen) >= 2) { p->used = 0; p->is_sn = 0; }
                    }
                }
            }
            return 1;
        }
        return 0;
    }
    /* ---- coordinador activo ---- */
    switch (h->type) {
    case MSG_HELLO:
        on_hello(n, from, r, now);
        return 1;
    case MSG_COORD_HB: case MSG_COORD_SYNC: case MSG_NEW_COORD: {
        uint32_t epoch = r_u32(r);
        if (r->err) return 0;
        /* solo un NEW_COORD (firmado con la clave de la partida, ya verificado, y no de un
           degradado: dispatch los descarta) nos baja */
        if (epoch > c->epoch) { if (h->type == MSG_NEW_COORD) step_down(n); }
        else if (epoch < c->epoch) {
            /* un coordinador viejo sigue hablando: se le informa la época vigente */
            uint8_t buf[160];
            int bl = auth_build_new_coord(n, c->epoch, buf, sizeof buf);
            if (bl) node_send_addr(n, from, buf, bl);
        }
        return 1;
    }
    }
    if (id >= SMNET_MAX_PLAYERS || !c->nodes[id].used) return 0;
    cnode_t* p = &c->nodes[id];
    /* el remitente ya está verificado (sesión etiquetada con su certificado): si cambió de
       dirección (NAT que remapeó), se actualiza */
    if (!smnet_addr_eq(from, &p->addr) && !smnet_addr_eq(from, &n->cfg.self_addr)) p->addr = *from;
    switch (h->type) {
    case MSG_GAP_REPORT:
        on_gap_report(n, id, r, now);
        return 1;
    case MSG_METRICS: {
        uint32_t epoch = r_u32(r);
        if (epoch > c->epoch) { step_down(n); return 1; }
        p->upload_kbps = r_u32(r);
        p->nat = r_u8(r);
        p->area = r_u16(r);
        (void)r_u16(r);
        int cnt = r_u8(r);
        if (cnt > SMNET_MAX_SN) cnt = SMNET_MAX_SN;
        int k = 0;
        for (int i = 0; i < cnt && !r->err; i++) {
            uint16_t sid = r_u16(r), ms = r_u16(r);
            if (r->err) break;
            p->rtt_id[k] = sid; p->rtt_ms[k] = ms; k++;
        }
        if (!r->err) p->n_rtt = (uint16_t)k;
        p->last_seen_ms = now;
        return 1;
    }
    case MSG_FAILOVER: {
        uint32_t epoch = r_u32(r);
        uint16_t old = r_u16(r), nw = r_u16(r);
        if (r->err) return 0;
        if (epoch > c->epoch) { step_down(n); return 1; }
        p->last_seen_ms = now;
        if (alive_sn(c, old) && old != nw) {
            /* dos peers distintos dicen que este SN dejó de responder: se lo da por caído ya */
            cnode_t* o = &c->nodes[old];
            if (now - o->suspect_ms > 2000) o->suspects = 0;
            o->suspects++;
            o->suspect_ms = now;
            if (o->suspects >= 2 && now - o->last_report_ms > 600) sn_dead(n, old, now);
        }
        if (alive_sn(c, nw)) p->primary = nw;
        p->backup = SMNET_NO_ID;
        uint16_t b = pick_sn(n, id, p->primary, 1);
        p->backup = b;
        send_assign(n, id);
        return 1;
    }
    case MSG_SN_REPORT: {
        uint32_t epoch = r_u32(r);
        uint32_t egress = r_u32(r);
        int cnt = r_u16(r);
        if (r->err) return 0;
        if (epoch > c->epoch) { step_down(n); return 1; }
        if (!p->is_sn) {
            /* sigue actuando como SN pero lo degradamos: se le repite */
            int pending = 0;
            for (int k = 0; k < c->npend; k++) if (c->pend[k].id == id) pending = 1;
            if (!pending) send_epoch_msg(n, id, MSG_DEMOTE, 0);
            return 1;
        }
        p->last_report_ms = now;
        p->last_seen_ms = now;
        p->got_report = 1;
        p->egress_kbps = egress;
        int hosted = 0;
        for (int i = 0; i < cnt && !r->err; i++) {
            uint16_t hid = r_u16(r);
            if (r->err || hid >= SMNET_MAX_PLAYERS) break;
            cnode_t* q = &c->nodes[hid];
            if (!q->used) continue;
            q->last_hosted_ms = now;
            hosted++;
            if (q->is_sn) continue;
            if (q->primary != id && !alive_sn(c, q->primary)) { q->primary = id; q->dirty = 1; }
            else if (q->primary != id && now - q->last_assign_ms > 1500 && now - q->last_ctl_ms > 1000) {
                /* lo aloja otro SN que el asignado: el ASSIGN se perdió, se reenvía */
                send_assign(n, hid);
                q->last_ctl_ms = now;
            }
        }
        /* el cupo se calcula con lo que cuesta cada peer alojado, sin el reenvío entre SN (que
           crece con la cantidad de SN y realimentaba la elección: más SN -> más reenvío -> menos
           cupo -> más SN) */
        uint32_t peer_egress = egress;
        if (r->pos + 4 <= r->len) peer_egress = r_u32(r);
        if (hosted > 0) {
            float per = (float)peer_egress / (float)hosted;
            if (per > 20 && per < 5000) c->est_down_kbps = 0.9f * c->est_down_kbps + 0.1f * per;
        }
        return 1;
    }
    }
    return 0;
}

void coord_update(smnet_node_t* n, double now) {
    coord_t* c = n->co;
    const smnet_coord_cfg_t* k = &n->cfg.coord;
    if (c->role == SMNET_COORD_REPLICA) {
        if (c->last_hb_rx > 0 && now - c->last_hb_rx > k->takeover_ms * (c->my_rank + 1)) {
            if (n->epoch_seen > c->epoch) { c->role = SMNET_COORD_NONE; c->last_hb_rx = 0; c->probe_at = 0; }   /* ya hay otro */
            /* antes de tomar el control, preguntarle al coordinador: si lo que pasó es que esta
               réplica estuvo unos segundos sin CPU, el coordinador ya la dio de baja (no le manda
               más latidos) pero sigue vivo; tomar el control partiría la partida en dos */
            else if (c->probe_at <= 0) { c->probe_at = now; c->probe_heard = 0; peer_probe_coord(n, now); }
            else if (c->probe_heard) { c->role = SMNET_COORD_NONE; c->last_hb_rx = 0; c->probe_at = 0; }
            else if (now - c->probe_at > 1500) { c->probe_at = 0; takeover(n, now); }
            else if (now - c->probe_at > 500 && now - c->probe_at < 520) peer_probe_coord(n, now);
        }
        return;
    }
    if (c->role != SMNET_COORD_ACTIVE) return;
    /* tras tomar el control: reenviar NEW_COORD a quien todavía no habló con nosotros */
    if (c->took_over_ms > 0 && now - c->took_over_ms < 6000 && now - c->last_newcoord_resend >= 500) {
        uint8_t buf[160];
        int bl = auth_build_new_coord(n, c->epoch, buf, sizeof buf);
        for (int i = 0; i < SMNET_MAX_PLAYERS && bl; i++) {
            cnode_t* p = &c->nodes[i];
            if (p->used && i != n->id && p->last_seen_ms <= c->took_over_ms + 1 && p->last_report_ms <= c->took_over_ms + 1)
                send_to_node(n, (uint16_t)i, buf, bl);
        }
        c->last_newcoord_resend = now;
    }
    /* latidos y sincronización a las réplicas */
    if (now - c->last_hb_tx >= k->hb_every_ms) {
        for (int r = 0; r < c->nreplicas; r++) send_replica_hb(n, c->replicas[r], (uint8_t)r);
        c->last_hb_tx = now;
    }
    if (now - c->last_sync >= k->sync_every_ms) {
        for (int r = 0; r < c->nreplicas; r++) send_sync(n, c->replicas[r], r);
        c->last_sync = now;
    }
    /* degradaciones diferidas */
    for (int i = 0; i < c->npend; i++) {
        if (now >= c->pend[i].at) {
            uint16_t id = c->pend[i].id;
            if (c->nodes[id].used && !c->nodes[id].is_sn) send_epoch_msg(n, id, MSG_DEMOTE, 0);
            c->pend[i--] = c->pend[--c->npend];
        }
    }
    /* caídas: se revisa cada segundo */
    if (now - c->last_live >= 1000 || now < c->last_live) {
        int before = c->snlist_dirty;
        liveness(n, now);
        c->last_live = now;
        if (c->snlist_dirty && !before) c->last_ondemand = 0;
    }
    if (now >= c->next_rebalance) {
        rebalance(n, now);
        c->next_rebalance = now + k->rebalance_ms;
    } else if (now - c->last_ondemand >= k->ondemand_gap_ms) {
        int any = 0;
        for (int i = 0; i < SMNET_MAX_PLAYERS && !any; i++) {
            cnode_t* p = &c->nodes[i];
            if (!p->used) continue;
            if (p->dirty || (!p->is_sn && (!alive_sn(c, p->primary) || !alive_sn(c, p->backup)))) any = 1;
        }
        if (any) {
            elect(n, now);
            fill_missing(n);
            for (int i = 0; i < SMNET_MAX_PLAYERS; i++) if (c->nodes[i].used && c->nodes[i].dirty) send_assign(n, (uint16_t)i);
            c->last_ondemand = now;
        }
    }
    if (c->snlist_dirty && now - c->last_snlist_bcast >= 200) broadcast_snlist(n, now);
}
