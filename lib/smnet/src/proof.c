/* Integridad del estado y detección de SN maliciosos (PROTOCOL.md §11.2–11.4, v1.4).
 *
 *   emisor   : una vez por segundo manda STATE_PROOF a su SN: hashes de 6 B de cada estado que
 *              mandó desde la prueba anterior (id, seq, 12 B del estado), firmados, + su certificado.
 *              Con un hash por estado, quien recibió solo algunos igual puede verificar los suyos.
 *   SN       : guarda la última prueba de cada entidad y la reenvía (PROOF_FWD) a los viewers que la
 *              tienen entre sus 8 más cercanos, y a los SN suscriptos a 30 Hz (RELAY_PROOF).
 *   viewer   : sigue a sus 8 vecinos más cercanos que le llegan por el SN, guarda el hash de cada
 *              estado recibido y lo compara con la prueba. Cuenta faltantes contra lo emitido.
 *              Reporta (GAP_REPORT) alteraciones, descarte sostenido y pruebas que no llegan.
 *   coord.   : decide (coord.c): 3 testigos distintos -> degradación permanente; la mitad de los
 *              alojados de un SN chico -> 10 minutos.
 *
 * Con encrypt = 0 no se firma ni se verifica la firma, pero los hashes sí se comparan: la detección
 * funciona igual en los bancos de escala. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "node_internal.h"
#include "../third_party/monocypher/monocypher.h"

#define PROOF_EVERY   1000.0
#define WINDOW_MS     5000.0
#define DROP_RATIO    0.50          /* faltantes en la ventana: un SN honesto entrega 60–95% de los
                                       estados emitidos (los que llegan en el mismo tick se reemplazan) */
#define DROP_MIN_ENT  3
#define NOPROOF_MS    3500.0
#define PROOF_STALE   1500.0

static void state_hash(uint16_t id, uint8_t seq, const uint8_t st[SMNET_STATE_SIZE], uint8_t out[PROOF_HASH]) {
    uint8_t m[3 + SMNET_STATE_SIZE];
    m[0] = (uint8_t)id; m[1] = (uint8_t)(id >> 8); m[2] = seq;
    memcpy(m + 3, st, SMNET_STATE_SIZE);
    crypto_blake2b(out, PROOF_HASH, m, sizeof m);
}

int proof_init(smnet_node_t* n) {
    verify_t* v = &n->peer.ver;
    memset(v, 0, sizeof *v);
    v->pk = malloc((size_t)SMNET_MAX_PLAYERS * 32);
    v->pk_ok = calloc(SMNET_MAX_PLAYERS, 1);
    return v->pk && v->pk_ok ? 0 : -1;
}

void proof_free(smnet_node_t* n) {
    verify_t* v = &n->peer.ver;
    free(v->pk); free(v->pk_ok);
    v->pk = NULL; v->pk_ok = NULL;
}

/* ------------------------------------------------------------- emisor */

void proof_note_sent(smnet_node_t* n, uint8_t seq, const uint8_t st[SMNET_STATE_SIZE]) {
    verify_t* v = &n->peer.ver;
    if (n->id >= SMNET_MAX_PLAYERS) return;
    if (v->nsent == 0) v->first_seq = seq;
    if (v->nsent >= PROOF_MAX) {   /* más de 32 sin prueba: se corre la ventana */
        memmove(v->sent_h, v->sent_h + 1, (size_t)(PROOF_MAX - 1) * PROOF_HASH);
        v->nsent = PROOF_MAX - 1;
        v->first_seq++;
    }
    state_hash(n->id, seq, st, v->sent_h[v->nsent]);
    v->nsent++;
}

static int proof_body(uint16_t id, uint8_t first, uint8_t cnt, const uint8_t* hashes, uint8_t* m) {
    memcpy(m, "smnet-proof", 11);
    m[11] = (uint8_t)id; m[12] = (uint8_t)(id >> 8); m[13] = first; m[14] = cnt;
    memcpy(m + 15, hashes, (size_t)cnt * PROOF_HASH);
    return 15 + cnt * PROOF_HASH;
}

static void send_proof(smnet_node_t* n) {
    verify_t* v = &n->peer.ver;
    peer_t* p = &n->peer;
    if (!v->nsent || !n->have_cert || p->primary.id == SMNET_NO_ID) return;
    uint8_t m[16 + PROOF_MAX * PROOF_HASH], sig[64];
    int ml = proof_body(n->id, v->first_seq, (uint8_t)v->nsent, &v->sent_h[0][0], m);
    link_sign(n->link, sig, m, ml);
    uint8_t buf[SMNET_HDR_SIZE + 2 + PROOF_MAX * PROOF_HASH + 64 + CERT_SIZE];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    node_hdr(n, &w, MSG_STATE_PROOF, 0, 0);
    w_u8(&w, v->first_seq);
    w_u8(&w, (uint8_t)v->nsent);
    w_bytes(&w, v->sent_h, v->nsent * PROOF_HASH);
    w_bytes(&w, sig, 64);
    w_bytes(&w, n->cert, CERT_SIZE);
    if (w.err) return;
    node_send_id(n, p->primary.id, &p->primary.addr, buf, w.len);
    if (p->next.id != SMNET_NO_ID) node_send_id(n, p->next.id, &p->next.addr, buf, w.len);
    v->nsent = 0;
}

/* ------------------------------------------------------------- viewer */

static vtrack_t* tracked(verify_t* v, uint16_t id) {
    for (int i = 0; i < v->ntr; i++) if (v->tr[i].id == id) return &v->tr[i];
    return NULL;
}

/* Una entrada vale mientras sea del mismo seq y reciente: seq es de 8 bits y da la vuelta cada
 * ~8,5 s a 30 Hz, así que algo anotado hace más de RH_MAX_AGE es de la vuelta anterior. */
#define RH_MAX_AGE 3000.0

static rh_t* slot_for(vtrack_t* t, uint8_t seq, double now) {
    rh_t* e = &t->ring[seq % VER_RING];
    if (!e->fl || e->seq != seq || now - e->t > RH_MAX_AGE) { memset(e, 0, sizeof *e); e->seq = seq; e->t = now; }
    return e;
}

static void send_report(smnet_node_t* n, uint8_t reason, const uint16_t* ids, int cnt);

/* Ya están los dos lados: se cuenta como entregado y se compara. */
static void match(smnet_node_t* n, vtrack_t* t, rh_t* e) {
    verify_t* v = &n->peer.ver;
    e->fl |= RH_DONE;
    t->got++;
    v->tot_got++;
    if (memcmp(e->h_rx, e->h_pf, PROOF_HASH) == 0) return;
    t->mism++;
    v->altered++;
    /* alteración: se reporta ya (con límite de 1 cada 2 s) */
    if (n->now - v->last_report > 2000) { uint16_t ids[1] = { t->id }; send_report(n, GAP_ALTERED, ids, 1); }
}

void proof_note_received(smnet_node_t* n, uint16_t id, uint8_t seq, const uint8_t st[SMNET_STATE_SIZE]) {
    verify_t* v = &n->peer.ver;
    vtrack_t* t = tracked(v, id);
    if (!t) return;
    t->rx++;
    rh_t* e = slot_for(t, seq, n->now);
    if (e->fl & (RH_RX | RH_DONE)) return;   /* repetido (dos SN, traspaso) */
    state_hash(id, seq, st, e->h_rx);
    e->fl |= RH_RX;
    if (e->fl & RH_PF) match(n, t, e);
}

static void send_report(smnet_node_t* n, uint8_t reason, const uint16_t* ids, int cnt) {
    verify_t* v = &n->peer.ver;
    if (n->now < v->quiet_until) return;
    uint8_t buf[SMNET_HDR_SIZE + 4 + 2 * VER_TRACK];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    node_hdr(n, &w, MSG_GAP_REPORT, 0, 0);
    w_u8(&w, reason);
    w_u16(&w, n->peer.primary.id);
    w_u8(&w, (uint8_t)cnt);
    for (int i = 0; i < cnt; i++) w_u16(&w, ids[i]);
    if (w.err) return;
    node_send_coord(n, buf, w.len);
    v->reports_sent++;
    v->last_report = n->now;
}

/* PROOF_FWD del SN: [eid u16][flags u8: 1 = trae certificado][cert][first u8][cnt u8][hashes][sig] */
int proof_on_forward(smnet_node_t* n, rbuf_t* r, double now) {
    verify_t* v = &n->peer.ver;
    uint16_t eid = r_u16(r);
    uint8_t flags = r_u8(r);
    uint8_t cert[CERT_SIZE];
    if (flags & 1) r_bytes(r, cert, CERT_SIZE);
    uint8_t first = r_u8(r), cnt = r_u8(r);
    if (r->err || eid >= SMNET_MAX_PLAYERS || cnt == 0 || cnt > PROOF_MAX) return 0;
    if (!n->cfg.verify_neighbors) return 1;   /* bots: no verifican */
    uint8_t hashes[PROOF_MAX * PROOF_HASH], sig[64];
    r_bytes(r, hashes, cnt * PROOF_HASH);
    r_bytes(r, sig, 64);
    if (r->err) return 0;
    if (flags & 1) {
        uint16_t cid = (uint16_t)(cert[0] | (cert[1] << 8));
        if (cid == eid && auth_check_cert(n, cert) == 0) { memcpy(v->pk + (size_t)eid * 32, cert + 2, 32); v->pk_ok[eid] = 1; }
    }
    vtrack_t* t = tracked(v, eid);
    if (!t) return 1;
    t->last_proof = now;
    /* la misma prueba puede llegar más de una vez (traspasos, dos SN): una sola cuenta */
    if (t->have_last && t->last_first == first && t->last_cnt == cnt) return 1;
    t->have_last = 1; t->last_first = first; t->last_cnt = cnt;
    if (!v->pk_ok[eid]) return 1;   /* todavía sin la clave de ese jugador */
    if (n->cfg.encrypt) {
        uint8_t m[16 + PROOF_MAX * PROOF_HASH];
        int ml = proof_body(eid, first, cnt, hashes, m);
        if (crypto_eddsa_check(sig, v->pk + (size_t)eid * 32, m, (size_t)ml) != 0) { v->proofs_bad++; return 1; }
    }
    v->proofs_ok++;
    /* anota lo firmado; se compara contra lo recibido apenas estén los dos */
    t->emitted += cnt;
    v->tot_emitted += cnt;
    for (int k = 0; k < cnt; k++) {
        rh_t* e = slot_for(t, (uint8_t)(first + k), now);
        if (e->fl & (RH_PF | RH_DONE)) continue;
        memcpy(e->h_pf, hashes + k * PROOF_HASH, PROOF_HASH);
        e->fl |= RH_PF;
        if (e->fl & RH_RX) match(n, t, e);
    }
    return 1;
}

static int cmp_dd2(const void* a, const void* b) { float x = ((const float*)a)[0], y = ((const float*)b)[0]; return (x > y) - (x < y); }

void proof_update(smnet_node_t* n, double now) {
    verify_t* v = &n->peer.ver;
    peer_t* p = &n->peer;
    if (p->status != SMNET_PEER_CONNECTED || n->id >= SMNET_MAX_PLAYERS) return;
    if (now >= v->next_proof) { send_proof(n); v->next_proof = now + PROOF_EVERY; }
    if (n->sn || !n->cfg.verify_neighbors) { v->ntr = 0; return; }   /* un SN no verifica a su propio camino (ni un bot) */
    /* cambió el SN que nos entrega: lo anotado viene del anterior; se empieza de cero y no se
       reporta por 3 s (lo que siga llegando del viejo no es culpa del nuevo) */
    if (p->primary.id != v->watch_sn) {
        v->watch_sn = p->primary.id;
        v->ntr = 0;
        v->window_start = now;
        v->quiet_until = now + 3000;
    }
    /* cada segundo: los 8 vecinos más cercanos que llegan por el SN */
    if (now >= v->next_track) {
        v->next_track = now + 1000;
        struct { float d; float id; } cand[SMNET_MAX_PLAYERS];
        int cnt = 0;
        for (int e = 0; e < SMNET_MAX_PLAYERS; e++) {
            const remote_t* rm = &p->remotes[e];
            if (e == n->id || !rm->valid || now - rm->last_rx_ms > 1000) continue;
            int direct = 0;
            for (int i = 0; i < p->dir.nl; i++) if (p->dir.l[i].id == e && p->dir.l[i].state == DL_UP) direct = 1;
            if (direct) continue;
            float dx = rm->st.x - p->local.x, dz = rm->st.z - p->local.z;
            cand[cnt].d = dx * dx + dz * dz; cand[cnt].id = (float)e; cnt++;
        }
        qsort(cand, (size_t)cnt, sizeof cand[0], cmp_dd2);
        vtrack_t old[VER_TRACK];
        int nold = v->ntr;
        memcpy(old, v->tr, sizeof old);
        v->ntr = 0;
        for (int k = 0; k < cnt && v->ntr < VER_TRACK; k++) {
            uint16_t id = (uint16_t)cand[k].id;
            vtrack_t* keep = NULL;
            for (int i = 0; i < nold; i++) if (old[i].id == id) keep = &old[i];
            if (keep) v->tr[v->ntr++] = *keep;
            else { vtrack_t* t = &v->tr[v->ntr++]; memset(t, 0, sizeof *t); t->id = id; t->since = now; t->last_proof = now; }
        }
        /* a nuestro SN: estos son los que verificamos */
        if (p->primary.id != SMNET_NO_ID) {
            uint8_t buf[SMNET_HDR_SIZE + 1 + 2 * VER_TRACK];
            wbuf_t w = { buf, 0, sizeof buf, 0 };
            node_hdr(n, &w, MSG_VERIFY_SET, 0, 0);
            w_u8(&w, (uint8_t)v->ntr);
            for (int i = 0; i < v->ntr; i++) w_u16(&w, v->tr[i].id);
            node_send_id(n, p->primary.id, &p->primary.addr, buf, w.len);
            if (p->next.id != SMNET_NO_ID) node_send_id(n, p->next.id, &p->next.addr, buf, w.len);
        }
    }
    /* ventana de 5 s: descarte sostenido y pruebas que no llegan */
    if (v->window_start == 0) v->window_start = now;
    if (now - v->window_start >= WINDOW_MS) {
        uint16_t drop_ids[VER_TRACK], np_ids[VER_TRACK];
        int nd = 0, nn = 0;
        for (int i = 0; i < v->ntr; i++) {
            vtrack_t* t = &v->tr[i];
            if (now - t->since < WINDOW_MS) continue;    /* recién entró al grupo */
            if (t->emitted >= 60 && t->got < (1.0 - DROP_RATIO) * t->emitted) drop_ids[nd++] = t->id;
            const remote_t* rm = &p->remotes[t->id];
            /* pruebas que no llegan: solo de vecinos que el SN nos entrega a ≥ 20 Hz (a esos los
               tiene suscriptos a 30 Hz y por lo tanto también recibe sus pruebas) */
            if (rm->valid && now - rm->last_rx_ms < 500 && now - t->last_proof > NOPROOF_MS && t->rx >= 20 * WINDOW_MS / 1000) np_ids[nn++] = t->id;
            t->emitted = t->got = 0;
            t->rx = 0;
        }
        if (nd >= DROP_MIN_ENT && now - v->last_report > 2000) send_report(n, GAP_DROP, drop_ids, nd);
        else if (nn >= DROP_MIN_ENT && now - v->last_report > 2000) send_report(n, GAP_NOPROOF, np_ids, nn);
        v->window_start = now;
    }
}

/* ----------------------------------------------------------------- SN */

/* Valor pseudoaleatorio estable por (a, b, c), para el SN malicioso de las pruebas. */
static float evil_rand(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ c * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return (float)(h & 0xFFFFFF) / 16777216.0f;
}

int sn_evil_drop(const smnet_node_t* n, uint16_t viewer, uint16_t e, uint8_t seq) {
    float r = n->cfg.sn.evil_drop;
    return r > 0 && evil_rand(viewer, e, seq) < r;
}

/* Guarda una prueba (de un alojado o reenviada por otro SN) para reenviarla. */
static void store_proof(sn_t* s, double now, uint16_t eid, const uint8_t* cert, uint8_t first, uint8_t cnt, const uint8_t* hashes, const uint8_t* sig) {
    ent_t* e = &s->ents[eid];
    if (e->has_cert && e->proof_first == first && e->proof_cnt == cnt && memcmp(e->proof_sig, sig, 64) == 0) return;   /* repetida */
    memcpy(e->cert, cert, CERT_SIZE);
    e->has_cert = 1;
    e->proof_first = first;
    e->proof_cnt = cnt;
    memcpy(e->proof_h, hashes, (size_t)cnt * PROOF_HASH);
    memcpy(e->proof_sig, sig, 64);
    e->proof_ver++;
    e->proof_t = now;
}

int sn_on_verify_set(smnet_node_t* n, const smnet_hdr_t* h, rbuf_t* r) {
    sn_t* s = n->sn;
    uint16_t id = h->sender;
    if (id >= SMNET_MAX_PLAYERS || !s->ents[id].hosted || !s->ents[id].v) return 0;
    view_t* vw = s->ents[id].v;
    int cnt = r_u8(r);
    if (cnt > VER_TRACK) cnt = VER_TRACK;
    for (int i = 0; i < cnt; i++) {
        uint16_t e = r_u16(r);
        if (r->err) return 0;
        /* uno nuevo en la lista: que reciba la última prueba ya (si la hay) */
        int had = 0;
        for (int k = 0; k < vw->nvset; k++) if (vw->vset[k] == e) had = 1;
        if (!had && e < SMNET_MAX_PLAYERS && s->ents[e].has_cert) s->ents[e].proof_fwd = (uint16_t)(s->ents[e].proof_ver - 1);
        vw->vset[i] = e;
    }
    vw->nvset = cnt;
    return 1;
}

int sn_on_proof(smnet_node_t* n, const smnet_hdr_t* h, rbuf_t* r, int relayed) {
    sn_t* s = n->sn;
    uint16_t eid = relayed ? r_u16(r) : h->sender;
    uint8_t cert[CERT_SIZE], hashes[PROOF_MAX * PROOF_HASH], sig[64];
    uint8_t first, cnt;
    if (relayed) r_bytes(r, cert, CERT_SIZE);
    first = r_u8(r); cnt = r_u8(r);
    if (r->err || eid >= SMNET_MAX_PLAYERS || cnt == 0 || cnt > PROOF_MAX) return 0;
    r_bytes(r, hashes, cnt * PROOF_HASH);
    r_bytes(r, sig, 64);
    if (!relayed) r_bytes(r, cert, CERT_SIZE);
    if (r->err) return 0;
    if ((uint16_t)(cert[0] | (cert[1] << 8)) != eid) return 0;
    if (!relayed && !s->ents[eid].hosted) return 0;
    if (relayed && s->ents[eid].hosted) return 1;   /* lo tengo de primera mano */
    store_proof(s, n->now, eid, cert, first, cnt, hashes, sig);
    return 1;
}

/* Cada tick: las pruebas nuevas a los viewers que tienen esa entidad entre sus 8 más cercanos
 * (con el certificado la primera vez) y a los SN suscriptos a 30 Hz. */
void sn_forward_proofs(smnet_node_t* n) {
    sn_t* s = n->sn;
    if (n->cfg.sn.evil_drop_proofs) return;
    uint8_t buf[SMNET_HDR_SIZE + 4 + CERT_SIZE + 2 + PROOF_MAX * PROOF_HASH + 64];
    for (int e = 0; e < SMNET_MAX_PLAYERS; e++) {
        ent_t* ee = &s->ents[e];
        if (!ee->has_cert || ee->proof_ver == ee->proof_fwd) continue;
        ee->proof_fwd = ee->proof_ver;
        /* una prueba de hace más de PROOF_STALE ya no sirve: su seq puede ser de la vuelta
           anterior (8 bits, ~8,5 s a 30 Hz) y haría ver como alterados estados legítimos */
        if (n->now - ee->proof_t > PROOF_STALE) continue;
        for (int v = 0; v < SMNET_MAX_PLAYERS; v++) {
            ent_t* ev = &s->ents[v];
            if (!ev->hosted || !ev->v || v == e) continue;
            view_t* vw = ev->v;
            int near = 0;
            for (int k = 0; k < vw->nvset; k++) if (vw->vset[k] == e) { near = 1; break; }
            if (!near) continue;
            int with_cert = !(vw->cert_sent[e >> 3] & (1 << (e & 7)));
            wbuf_t w = { buf, 0, sizeof buf, 0 };
            node_hdr(n, &w, MSG_PROOF_FWD, 0, 0);
            w_u16(&w, (uint16_t)e);
            w_u8(&w, (uint8_t)with_cert);
            if (with_cert) w_bytes(&w, ee->cert, CERT_SIZE);
            w_u8(&w, ee->proof_first);
            w_u8(&w, ee->proof_cnt);
            w_bytes(&w, ee->proof_h, ee->proof_cnt * PROOF_HASH);
            w_bytes(&w, ee->proof_sig, 64);
            if (w.err) continue;
            node_send_id(n, (uint16_t)v, &ev->addr, buf, w.len);
            vw->cert_sent[e >> 3] |= (uint8_t)(1 << (e & 7));
        }
        if (!ee->hosted) continue;
        for (int oi = 0; oi < s->nothers; oi++) {
            other_t* o = &s->others[oi];
            if ((o->sub[e] & 0x7F) != 2 /* TIER_30 */) continue;
            wbuf_t w = { buf, 0, sizeof buf, 0 };
            node_hdr(n, &w, MSG_RELAY_PROOF, 0, 0);
            w_u16(&w, (uint16_t)e);
            w_bytes(&w, ee->cert, CERT_SIZE);
            w_u8(&w, ee->proof_first);
            w_u8(&w, ee->proof_cnt);
            w_bytes(&w, ee->proof_h, ee->proof_cnt * PROOF_HASH);
            w_bytes(&w, ee->proof_sig, 64);
            if (!w.err) node_send_id(n, o->id, &o->addr, buf, w.len);
        }
    }
}
