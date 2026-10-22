/* Lista de jugadores y dueño de área (API v1, PROTOCOL.md §6.5).
 *
 *   peer   -> coord : PLAYER_INFO [snapshot u8][len u8][info]   (al entrar, al cambiar, tras NEW_COORD)
 *   peer   -> coord : AREA_NOTICE [area u16]                    (al cambiar de área)
 *   coord  -> SN    : ROSTER / AREA_OWNER (flags 0): el SN los reparte a sus alojados (flags 1)
 *   coord  -> peer  : ROSTER (flags 1) con la lista completa, a quien entra (snapshot = 1)
 *
 * ROSTER     [flags u8][n u8] n × ([id u16][alta u8][len u8][info]) [total u16][0xA7]?
 *
 * Los ROSTER van sin reintento: si se pierde uno, falta (o sobra) alguien para siempre. Por eso
 * el coordinador manda cada 2 s un ROSTER vacío con el total al final, y el peer que no llega a
 * ese total por más de 4 s pide la lista completa (PLAYER_INFO con snapshot = 1, cada 10 s como
 * mucho); con la lista completa también quita a los que sobran.
 * AREA_OWNER [flags u8][area u16][dueño u16]
 *
 * El dueño de un área lo elige el coordinador entre los jugadores que están en ella (se queda el que
 * ya era mientras siga ahí) y se le avisa a cada jugador del área, directo, cuando cambia o cuando
 * llega al área. Los cambios son raros: no hace falta pasar por los SN. */
#include <string.h>
#include <stdlib.h>
#include "node_internal.h"

#define RF_FINAL 1
#define ROSTER_PER_MSG 14
#define ROSTER_TOTAL_TAG 0xA7
#define ROSTER_TOTAL_EVERY 2000.0

static int from_authority(const smnet_node_t* n, uint16_t sender, int local) {
    if (local) return 1;
    if (n->have_coord && sender == n->coord_id) return 1;
    for (int i = 0; i < n->nsn; i++) if (n->snlist[i].id == sender) return !node_is_banned(n, sender);
    return 0;
}

/* --------------------------------------------------------------- peer */

void smnet_node_set_player_info(smnet_node_t* n, const uint8_t* info, int len) {
    if (len < 0) len = 0;
    if (len > SMNET_MAX_PINFO) len = SMNET_MAX_PINFO;
    memcpy(n->pinfo, info, (size_t)len);
    n->pinfo_len = (uint8_t)len;
    n->pinfo_dirty = 1;
}

uint16_t smnet_node_area_owner(const smnet_node_t* n) {
    return n->owner_area == n->peer.area ? n->area_owner : SMNET_NO_ID;
}

static void roster_repair(smnet_node_t* n);
static int repair_off(void);
static void send_pinfo(smnet_node_t* n, int snapshot) {
    uint8_t buf[SMNET_HDR_SIZE + 2 + SMNET_MAX_PINFO];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    node_hdr(n, &w, MSG_PLAYER_INFO, 0, 0);
    w_u8(&w, (uint8_t)snapshot);
    w_u8(&w, n->pinfo_len);
    w_bytes(&w, n->pinfo, n->pinfo_len);
    if (!w.err) node_send_coord(n, buf, w.len);
}

void roster_peer_update(smnet_node_t* n) {
    peer_t* p = &n->peer;
    if (p->status != SMNET_PEER_CONNECTED || !n->have_coord || n->id >= SMNET_MAX_PLAYERS) return;
    if (n->roster_epoch != n->epoch_seen) {          /* recién conectados, o coordinador nuevo */
        int first = n->roster_epoch == 0;
        n->roster_epoch = n->epoch_seen;
        send_pinfo(n, first);
        n->pinfo_dirty = 0;
        n->told_area = 0xFFFFFFFFu;
    } else if (n->pinfo_dirty) { send_pinfo(n, 0); n->pinfo_dirty = 0; }
    /* huérfanos: se anunció nuestra baja, o hace 8 s que no llega el total que el coordinador
       manda cada 2 s (nuestro SN fue dado por muerto, por ejemplo tras unos segundos sin CPU). Solo
       si alguna vez llegó (un coordinador viejo no lo manda) y no siendo el coordinador activo. */
    /* 20 s y un poco más según el nodo: que no vuelvan todos a la vez */
    int orphan = n->roster_total_at > 0 && n->now - n->roster_total_at > 10 * ROSTER_TOTAL_EVERY + (double)(n->cfg.nonce % 5000u) &&
                 !(n->co && n->co->role == SMNET_COORD_ACTIVE);
    if (repair_off()) { orphan = 0; n->roster_self_gone = 0; }
    if (n->roster_self_gone || orphan) {
        n->roster_self_gone = 0;
        n->roster_total_at = n->now;   /* sigue vigilando: si tampoco llega tras volver, otra vez */
        n->roster_collect = 0; n->roster_mis_since = 0;
        peer_force_rejoin(n, n->now);
        return;
    }
    roster_repair(n);
    if (n->told_area != p->area) {
        uint8_t buf[SMNET_HDR_SIZE + 2];
        wbuf_t w = { buf, 0, sizeof buf, 0 };
        node_hdr(n, &w, MSG_AREA_NOTICE, 0, 0);
        w_u16(&w, p->area);
        node_send_coord(n, buf, w.len);
        n->told_area = p->area;
    }
}

static void deliver_roster(smnet_node_t* n, rbuf_t* r, int cnt) {
    for (int i = 0; i < cnt; i++) {
        uint16_t id = r_u16(r);
        int alta = r_u8(r), len = r_u8(r);
        if (r->err || id >= SMNET_MAX_PLAYERS || len > SMNET_MAX_PINFO || r->pos + len > r->len) return;
        const uint8_t* info = r->p + r->pos;
        r->pos += len;
        uint8_t* k = &n->known[id >> 3];
        uint8_t bit = (uint8_t)(1 << (id & 7));
        if (id == n->id) {   /* uno mismo: si se anuncia nuestra baja, quedamos huérfanos */
            if (!alta) { n->roster_self_gone = 1; }
            continue;
        }
        if (alta) { *k |= bit; if (n->on_player) n->on_player(n->user, id, 1, info, len); }
        else if (*k & bit) { *k &= (uint8_t)~bit; if (n->on_player) n->on_player(n->user, id, 0, NULL, 0); }
        if (n->roster_collect) { if (alta) n->roster_seen[id >> 3] |= bit; else n->roster_seen[id >> 3] &= (uint8_t)~bit; }
    }
    /* el total, al final (ver arriba) */
    if (r->len - r->pos == 3 && r->p[r->pos + 2] == ROSTER_TOTAL_TAG) {
        uint16_t t = r_u16(r);
        if (!r->err) { n->roster_total = t; n->roster_total_at = n->now; }
    }
}

static int known_count(const smnet_node_t* n, const uint8_t* bits) {
    int c = 0;
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) if (i != n->id && (bits[i >> 3] >> (i & 7) & 1)) c++;
    return c + 1;   /* uno mismo */
}

/* ¿la lista coincide con el total que anuncia el coordinador? si no, se pide entera */
/* SMNET_SIN_REPARAR=1: sin la reparación de la lista ni la vuelta de los huérfanos (para comparar) */
static int repair_off(void) { static int v = -1; if (v < 0) { const char* e = getenv("SMNET_SIN_REPARAR"); v = (e && e[0] == '1'); } return v; }
static void roster_repair(smnet_node_t* n) {
    double now = n->now;
    if (repair_off()) return;
    if (n->roster_collect && now - n->roster_collect_at > 3000) {
        n->roster_collect = 0;
        /* con la lista completa a la vista: los que sobran se van (una baja perdida) */
        if (n->roster_total && known_count(n, n->roster_seen) >= n->roster_total) {
            for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
                uint8_t bit = (uint8_t)(1 << (i & 7));
                if (i == n->id || !(n->known[i >> 3] & bit) || (n->roster_seen[i >> 3] & bit)) continue;
                n->known[i >> 3] &= (uint8_t)~bit;
                if (n->on_player) n->on_player(n->user, (uint16_t)i, 0, NULL, 0);
            }
        }
    }
    if (!n->roster_total || now - n->roster_total_at > 3 * ROSTER_TOTAL_EVERY) return;
    if (known_count(n, n->known) == n->roster_total) { n->roster_mis_since = 0; n->roster_req_fails = 0; return; }
    if (n->roster_mis_since <= 0) { n->roster_mis_since = now; return; }
    if (now - n->roster_mis_since < 4000 || now - n->roster_snap_req < 10000 || n->roster_collect) return;
    /* dos pedidos seguidos sin que la cuenta cierre: el coordinador no nos tiene (perdimos nuestra
       baja estando sin CPU, por ejemplo) y no contesta. Volver a entrar. */
    /* (solo cuenta si el total no se movió: mientras entra gente, no cierra sin que haya nada roto) */
    if (n->roster_total != n->roster_req_total) { n->roster_req_total = n->roster_total; n->roster_req_fails = 0; }
    if (++n->roster_req_fails > 2 && !(n->co && n->co->role == SMNET_COORD_ACTIVE)) { n->roster_req_fails = 0; n->roster_mis_since = 0; peer_force_rejoin(n, now); return; }
    n->roster_snap_req = now;
    n->roster_collect = 1; n->roster_collect_at = now;
    memset(n->roster_seen, 0, sizeof n->roster_seen);
    send_pinfo(n, 1);
}

/* ROSTER / AREA_OWNER: en un SN se reparten (flags 0, del coordinador); en un peer se entregan */
int roster_receive(smnet_node_t* n, const smnet_hdr_t* h, rbuf_t* r, int local) {
    if (!from_authority(n, h->sender, local)) { n->stats.unauthorized++; return 0; }
    uint8_t flags = r_u8(r);
    if (r->err) return 0;
    if (!(flags & RF_FINAL)) {
        /* reparto en el SN: solo si viene del coordinador */
        if (!n->sn || !(local || (n->have_coord && h->sender == n->coord_id))) return 0;
        uint8_t buf[SMNET_MTU];
        int body = r->len - r->pos;
        if (body < 0 || SMNET_HDR_SIZE + 1 + body > (int)sizeof buf) return 0;
        wbuf_t w = { buf, 0, sizeof buf, 0 };
        node_hdr(n, &w, h->type, 0, 0);
        w_u8(&w, RF_FINAL);
        w_bytes(&w, r->p + r->pos, body);
        uint16_t area = SMNET_NO_ID;
        if (h->type == MSG_AREA_OWNER) { rbuf_t r2 = *r; area = r_u16(&r2); }
        for (int e = 0; e < SMNET_MAX_PLAYERS; e++) {
            ent_t* ee = &n->sn->ents[e];
            if (!ee->hosted) continue;
            if (area != SMNET_NO_ID && ee->area != area) continue;
            node_send_id(n, (uint16_t)e, &ee->addr, buf, w.len);
        }
        return 1;
    }
#ifdef SMNET_PRUEBA_PERDER_ROSTER   /* solo para probar la reparación: se pierde el 30 % */
    if (h->type == MSG_ROSTER) { static unsigned s = 1; s = s * 1103515245u + 12345u; if ((s >> 16) % 100 < 30) return 1; }
#endif
    if (h->type == MSG_ROSTER) { int cnt = r_u8(r); if (!r->err) deliver_roster(n, r, cnt); return 1; }
    uint16_t area = r_u16(r), owner = r_u16(r);
    if (r->err) return 0;
    if (n->owner_area != area || n->area_owner != owner) {
        n->owner_area = area; n->area_owner = owner;
        if (n->on_owner && area == n->peer.area) n->on_owner(n->user, area, owner);
    }
    return 1;
}

/* -------------------------------------------------------- coordinador */

static void to_sns(smnet_node_t* n, const uint8_t* buf, int len) {
    coord_t* c = n->co;
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++)
        if (c->nodes[i].used && c->nodes[i].is_sn && !(c->nodes[i].banned_until > n->now)) node_send_id(n, (uint16_t)i, &c->nodes[i].addr, buf, len);
}

/* PLAYER_INFO / AREA_NOTICE */
int roster_coord_receive(smnet_node_t* n, const smnet_hdr_t* h, rbuf_t* r) {
    coord_t* c = n->co;
    if (!c || c->role != SMNET_COORD_ACTIVE || h->sender >= SMNET_MAX_PLAYERS) return 0;
    cnode_t* p = &c->nodes[h->sender];
    if (!p->used) return 0;
    if (h->type == MSG_AREA_NOTICE) {
        uint16_t a = r_u16(r);
        if (r->err) return 0;
        p->area = a;
        p->owner_told = 0;
        return 1;
    }
    int snap = r_u8(r), len = r_u8(r);
    if (r->err || len > SMNET_MAX_PINFO) return 0;
    r_bytes(r, p->pinfo, len);
    if (r->err) return 0;
    p->pinfo_len = (uint8_t)len;
    p->pinfo_dirty = 1;
    if (snap) p->want_snapshot = 1;
    return 1;
}

static void flush_entries(smnet_node_t* n, uint8_t* buf, wbuf_t* w, int* cnt, int cpos, uint16_t direct) {
    if (!*cnt) return;
    buf[cpos] = (uint8_t)*cnt;
    if (direct == SMNET_NO_ID) to_sns(n, buf, w->len);
    else node_send_id(n, direct, &n->co->nodes[direct].addr, buf, w->len);
    *cnt = 0;
}

static void start_roster(smnet_node_t* n, uint8_t* buf, wbuf_t* w, int* cpos, int final) {
    *w = (wbuf_t){ buf, 0, SMNET_MTU, 0 };
    node_hdr(n, w, MSG_ROSTER, 0, 0);
    w_u8(w, (uint8_t)(final ? RF_FINAL : 0));
    *cpos = w->len;
    w_u8(w, 0);
}

/* el coordinador activo es la autoridad: su propia lista sale de su tabla, sin depender de que un
   SN se la reenvíe (si ese SN murió, la lista del coordinador quedaba vacía) */
static void self_note(smnet_node_t* n, uint16_t i, int alta, const uint8_t* info, int len) {
    if (i == n->id || i >= SMNET_MAX_PLAYERS) return;
    uint8_t* k = &n->known[i >> 3];
    uint8_t bit = (uint8_t)(1 << (i & 7));
    if (alta) { *k |= bit; if (n->on_player) n->on_player(n->user, i, 1, info, len); }
    else if (*k & bit) { *k &= (uint8_t)~bit; if (n->on_player) n->on_player(n->user, i, 0, NULL, 0); }
}

void roster_coord_update(smnet_node_t* n, double now) {
    coord_t* c = n->co;
    if (c->role != SMNET_COORD_ACTIVE || now - c->roster_last < 250) return;
    c->roster_last = now;
    uint8_t buf[SMNET_MTU];
    wbuf_t w; int cpos, cnt = 0;
    /* altas, cambios y bajas: a todos por los SN */
    start_roster(n, buf, &w, &cpos, 0);
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
        cnode_t* p = &c->nodes[i];
        int alta = p->used && p->pinfo_dirty;
        int baja = !p->used && p->announced;
        if (!alta && !baja) continue;
        w_u16(&w, (uint16_t)i); w_u8(&w, (uint8_t)alta);
        w_u8(&w, alta ? p->pinfo_len : 0);
        if (alta) w_bytes(&w, p->pinfo, p->pinfo_len);
        if (alta) { p->pinfo_dirty = 0; p->announced = 1; } else p->announced = 0;
        self_note(n, (uint16_t)i, alta, p->pinfo, p->pinfo_len);
        if (++cnt == ROSTER_PER_MSG) { flush_entries(n, buf, &w, &cnt, cpos, SMNET_NO_ID); start_roster(n, buf, &w, &cpos, 0); }
    }
    flush_entries(n, buf, &w, &cnt, cpos, SMNET_NO_ID);
    /* el total, cada 2 s: un ROSTER vacío con [total u16][0xA7] al final (para reparar listas) */
    if (now - c->roster_total_last >= ROSTER_TOTAL_EVERY) {
        c->roster_total_last = now;
        int tot = 0;
        for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
            cnode_t* p = &c->nodes[i];
            int on = p->used && p->announced;
            tot += on;
            /* y la propia lista, igual a la tabla */
            int has = (n->known[i >> 3] >> (i & 7)) & 1;
            if (on != has) self_note(n, (uint16_t)i, on, p->pinfo, p->pinfo_len);
        }
        start_roster(n, buf, &w, &cpos, 0);
        w_u16(&w, (uint16_t)tot); w_u8(&w, ROSTER_TOTAL_TAG);
        to_sns(n, buf, w.len);
    }
    /* lista completa a quien acaba de entrar */
    /* como mucho 8 por vuelta (32/s): si entran o vuelven cientos juntos, el resto espera turno
       en vez de que el coordinador mande miles de mensajes de golpe */
    int snaps = 0;
    for (int j = 0; j < SMNET_MAX_PLAYERS; j++) {
        cnode_t* q = &c->nodes[j];
        if (!q->used || !q->want_snapshot) continue;
        if (++snaps > 8) break;
        q->want_snapshot = 0;
        start_roster(n, buf, &w, &cpos, 1);
        for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
            cnode_t* p = &c->nodes[i];
            if (!p->used || !p->announced || i == j) continue;
            w_u16(&w, (uint16_t)i); w_u8(&w, 1); w_u8(&w, p->pinfo_len); w_bytes(&w, p->pinfo, p->pinfo_len);
            if (++cnt == ROSTER_PER_MSG) { flush_entries(n, buf, &w, &cnt, cpos, (uint16_t)j); start_roster(n, buf, &w, &cpos, 1); }
        }
        flush_entries(n, buf, &w, &cnt, cpos, (uint16_t)j);
    }
    /* dueño por área: se queda el vigente mientras siga en el área */
    for (int k = 0; k < c->nareas; k++) c->areas[k].seen = 0;
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
        cnode_t* p = &c->nodes[i];
        if (!p->used || !p->announced) continue;
        int k = 0;
        while (k < c->nareas && c->areas[k].area != p->area) k++;
        if (k == c->nareas) {
            if (c->nareas == COORD_MAX_AREAS) continue;
            c->areas[k].area = p->area; c->areas[k].owner = SMNET_NO_ID; c->nareas++;
        }
        c->areas[k].seen = 1;
        uint16_t o = c->areas[k].owner;
        int valid = o < SMNET_MAX_PLAYERS && c->nodes[o].used && c->nodes[o].area == p->area;
        if (!valid) c->areas[k].owner = (uint16_t)i;
    }
    for (int k = 0; k < c->nareas; k++) if (!c->areas[k].seen) { c->areas[k--] = c->areas[--c->nareas]; }
    /* a cada jugador, directo, cuando el dueño de su área no es el último que se le dijo */
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) {
        cnode_t* p = &c->nodes[i];
        if (!p->used || !p->announced) continue;
        for (int k = 0; k < c->nareas; k++) {
            if (c->areas[k].area != p->area) continue;
            if (p->owner_told && p->told_owner == c->areas[k].owner && p->told_owner_area == p->area) break;
            wbuf_t a = { buf, 0, SMNET_MTU, 0 };
            node_hdr(n, &a, MSG_AREA_OWNER, 0, 0);
            w_u8(&a, RF_FINAL); w_u16(&a, p->area); w_u16(&a, c->areas[k].owner);
            node_send_id(n, (uint16_t)i, &p->addr, buf, a.len);
            p->owner_told = 1; p->told_owner = c->areas[k].owner; p->told_owner_area = p->area;
            break;
        }
    }
}
