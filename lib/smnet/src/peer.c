/* Rol peer: entrada, STATE_UP al primario, backup con latidos, failover, medición de RTT
 * a los SN y métricas al coordinador. */
#include <stdlib.h>
#include <string.h>
#include "node_internal.h"

static const ref_t NOREF = { SMNET_NO_ID, { 0, { 0 } } };

void peer_init(smnet_node_t* n) {
    peer_t* p = &n->peer;
    memset(p, 0, sizeof *p);
    p->primary = NOREF;
    p->backup = NOREF;
    p->alt = NOREF;
    p->next = NOREF;
    p->remotes = calloc(SMNET_MAX_PLAYERS, sizeof(remote_t));
    p->rtt = calloc(SMNET_MAX_PLAYERS, sizeof(uint16_t));
    if (direct_init(n) != 0 || proof_init(n) != 0) { free(p->rtt); p->rtt = NULL; }
}

void peer_free(smnet_node_t* n) {
    free(n->peer.remotes);
    free(n->peer.rtt);
    direct_free(n);
    proof_free(n);
}

static void send_hello(smnet_node_t* n, double now) {
    uint8_t buf[32];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    node_hdr(n, &w, MSG_HELLO, 0, 0);
    w_u8(&w, SMNET_PROTO_VERSION);
    w_u8(&w, SMNET_PROTO_VERSION);
    w_u32(&w, n->cfg.nonce);
    w_u32(&w, n->cfg.upload_kbps);
    w_u8(&w, n->cfg.nat);
    w_u16(&w, n->cfg.requested_id);   /* v1.4, opcional */
    node_send_coord(n, buf, w.len);
    n->peer.last_hello = now;
}

static void send_state(smnet_node_t* n) {
    peer_t* p = &n->peer;
    uint8_t buf[SMNET_MTU];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    p->seq++;
    int had_blob = p->blob_pending;
    node_hdr(n, &w, MSG_STATE_UP, (uint8_t)(p->blob_pending ? UP_HAS_BLOB : 0), p->seq);
    w_u16(&w, p->area);
    state_write(&w, &p->local);
    int short_len = w.len;
    proof_note_sent(n, (uint8_t)p->seq, buf + short_len - SMNET_STATE_SIZE);
    if (p->blob_pending) {
        w_u16(&w, (uint16_t)p->blob_len);
        w_bytes(&w, p->blob, p->blob_len);
        p->blob_pending = 0;
    }
    if (!w.err && p->dir.nl) {
        /* mismo contenido por los atajos directos: con y sin estado rico */
        uint8_t dshort[SMNET_HDR_SIZE + 16], dlong[SMNET_MTU];
        memcpy(dshort, buf, (size_t)short_len);
        dshort[0] = (uint8_t)(dshort[0] & 0xF0);
        dshort[1] = MSG_STATE_DIRECT;
        if (had_blob) { memcpy(dlong, buf, (size_t)w.len); dlong[1] = MSG_STATE_DIRECT; }
        direct_send_state(n, dshort, short_len, had_blob ? dlong : NULL, w.len);
    }
    if (!w.err) {
        node_send_id(n, p->primary.id, &p->primary.addr, buf, w.len);
        if (p->alt.id != SMNET_NO_ID && n->now < p->alt_until) node_send_id(n, p->alt.id, &p->alt.addr, buf, w.len);
        if (p->next.id != SMNET_NO_ID) node_send_id(n, p->next.id, &p->next.addr, buf, w.len);
    }
}

static void switch_primary(smnet_node_t* n, const ref_t* to, double now);

static void send_simple(smnet_node_t* n, uint8_t type, uint8_t flags, const ref_t* to) {
    uint8_t buf[SMNET_HDR_SIZE];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    node_hdr(n, &w, type, flags, 0);
    node_send_id(n, to->id, &to->addr, buf, w.len);
}

static void send_metrics(smnet_node_t* n) {
    peer_t* p = &n->peer;
    uint8_t buf[SMNET_MTU];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    node_hdr(n, &w, MSG_METRICS, 0, 0);
    w_u32(&w, n->epoch_seen);
    w_u32(&w, n->cfg.upload_kbps);
    w_u8(&w, n->cfg.nat);
    w_u16(&w, p->area);
    w_u16(&w, p->primary.id);
    int cnt_pos = w.len, cnt = 0;
    w_u8(&w, 0);
    for (int i = 0; i < n->nsn && cnt < SMNET_MAX_SN; i++) {
        uint16_t id = n->snlist[i].id;
        if (id >= SMNET_MAX_PLAYERS || p->rtt[id] == 0) continue;
        w_u16(&w, id);
        w_u16(&w, p->rtt[id]);
        cnt++;
    }
    buf[cnt_pos] = (uint8_t)cnt;
    if (!w.err) node_send_coord(n, buf, w.len);
}

static int best_known_sn(smnet_node_t* n, uint16_t ex1, uint16_t ex2) {
    peer_t* p = &n->peer;
    int best = -1; uint16_t brtt = 0xFFFF;
    for (int i = 0; i < n->nsn; i++) {
        const sninfo_t* si = &n->snlist[i];
        if (si->id == ex1 || si->id == ex2 || si->id >= SMNET_MAX_PLAYERS) continue;
        if (n->cfg.nat == SMNET_NAT_SYMMETRIC && !(si->flags & SNF_OPEN)) continue;
        uint16_t r = p->rtt[si->id];
        if (r == 0) continue;
        if (r < brtt) { brtt = r; best = i; }
    }
    return best;
}

static void send_failover_notice(smnet_node_t* n, uint16_t old, uint16_t nw) {
    uint8_t buf[32];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    node_hdr(n, &w, MSG_FAILOVER, 0, 0);
    w_u32(&w, n->epoch_seen);
    w_u16(&w, old);
    w_u16(&w, nw);
    node_send_coord(n, buf, w.len);
}

static void failover(smnet_node_t* n, double now) {
    peer_t* p = &n->peer;
    /* el backup tiene que haber respondido el último latido (cada backup_hb_ms) */
    if (p->backup.id != SMNET_NO_ID && now - p->last_rx_backup < n->cfg.backup_hb_ms * 1.6 + 200) {
        ref_t old = p->primary;
        p->primary = p->backup;
        p->backup = NOREF;
        p->last_rx_primary = now;
        p->primary_ok = 0;
        p->failovers++;
        /* failover en paralelo: si el backup también cayó, el segundo candidato ya nos aloja */
        int alt = best_known_sn(n, old.id, p->primary.id);
        if (alt >= 0) {
            p->alt.id = n->snlist[alt].id;
            p->alt.addr = n->snlist[alt].addr;
            p->alt_until = now + n->cfg.primary_timeout_ms + 500;
            uint16_t br = p->rtt[p->primary.id];
            double wait = br ? 2.0 * br + 60 : 300;
            p->alt_after = now + (wait < 400 ? wait : 400);
        }
        send_failover_notice(n, old.id, p->primary.id);
        p->last_failover_ms = now; p->last_failover_kind = 1;
        send_state(n);   /* el backup (y el alternativo) empiezan a alojarnos ya */
    } else {
        /* sin backup vivo: saltar directo al SN de menor RTT medido (el coordinador se entera
           por el FAILOVER y por los reportes del SN, y lo reacomoda en el próximo ciclo) */
        int best = best_known_sn(n, p->primary.id, p->alt.id);
        if (best >= 0) {
            ref_t old = p->primary;
            p->primary.id = n->snlist[best].id;
            p->primary.addr = n->snlist[best].addr;
            p->backup = NOREF;
            p->alt = NOREF;
            p->last_rx_primary = now;
            p->primary_ok = 0;
            p->failovers++;
            send_failover_notice(n, old.id, p->primary.id);
            p->last_failover_ms = now; p->last_failover_kind = 2;
            send_state(n);
            return;
        }
        /* nada conocido: volver a entrar por el coordinador (mismo nonce, mismo id) */
        p->status = SMNET_PEER_JOINING;
        p->primary = NOREF;
        p->backup = NOREF;
        p->rejoins++;
        n->roster_epoch = 0;   /* al volver, los datos de presentación y la lista entera otra vez */
        if (n->roster_total_at > 0) n->roster_total_at = now;
        send_hello(n, now);
    }
}

/* el coordinador nos dio por idos (se anunció nuestra baja) aunque el SN nos siga atendiendo:
   volver a entrar por el coordinador (mismo nonce, mismo id) y pedir la lista entera */
void peer_force_rejoin(smnet_node_t* n, double now) {
    peer_t* p = &n->peer;
    if (p->status != SMNET_PEER_CONNECTED) return;
    p->status = SMNET_PEER_JOINING;
    p->primary = NOREF;
    p->backup = NOREF;
    p->rejoins++;
    n->roster_epoch = 0;
    send_hello(n, now);
}

/* una réplica sin latidos pregunta si el coordinador sigue (un HELLO: si vive, contesta) */
void peer_probe_coord(smnet_node_t* n, double now) { send_hello(n, now); }

void peer_update(smnet_node_t* n, double now) {
    peer_t* p = &n->peer;
    const smnet_node_cfg_t* c = &n->cfg;
    if (p->status == SMNET_PEER_IDLE) return;
    if (p->status == SMNET_PEER_JOINING) {
        if (now - p->last_hello >= c->hello_retry_ms) send_hello(n, now);
        return;
    }
    if (p->primary.id == SMNET_NO_ID || now - p->last_rx_primary > c->primary_timeout_ms) {
        failover(n, now);
        if (p->status != SMNET_PEER_CONNECTED) return;
    }
    if (now >= p->next_tick) {
        p->tick++;
        send_state(n);
        p->next_tick += TICK_MS;
        if (p->next_tick < now) p->next_tick = now + TICK_MS;
    }
    if (p->backup.id != SMNET_NO_ID && now - p->last_hb_backup >= c->backup_hb_ms) {
        send_simple(n, MSG_HEARTBEAT, 0, &p->backup);
        p->last_hb_backup = now;
    }
    if (p->next.id != SMNET_NO_ID && (now > p->next_until || node_is_banned(n, p->primary.id))) { ref_t nx = p->next; switch_primary(n, &nx, now); }
    direct_update(n, now);
    proof_update(n, now);
    if (n->nsn > 0 && now >= p->next_ping) {
        if (p->ping_idx >= n->nsn) p->ping_idx = 0;
        const sninfo_t* s = &n->snlist[p->ping_idx++];
        /* NAT simétrico solo puede usar SN con NAT abierto: medir los demás es perder handshakes */
        int usable = n->cfg.nat != SMNET_NAT_SYMMETRIC || (s->flags & SNF_OPEN);
        if (s->id != n->id && usable && !(s->flags & SNF_OPEN) && s->id != p->primary.id &&
            p->primary.id != SMNET_NO_ID && p->status == SMNET_PEER_CONNECTED && !node_has_link(n, &s->addr)) {
            /* SN con NAT cono sin sesión: que nuestro primario le pase nuestra dirección para que
               abra su filtro (si no, su NAT descarta el PING); el INIT se reintenta ~4 s */
            uint8_t ib[SMNET_HDR_SIZE + 3];
            wbuf_t iw = { ib, 0, sizeof ib, 0 };
            node_hdr(n, &iw, MSG_INTRO_REQ, 0, 0);
            w_u8(&iw, 1);
            w_u16(&iw, s->id);
            node_send_id(n, p->primary.id, &p->primary.addr, ib, iw.len);
        }
        if (s->id != n->id && usable) {
            uint8_t buf[16];
            wbuf_t w = { buf, 0, sizeof buf, 0 };
            node_hdr(n, &w, MSG_PING, 0, 0);
            w_u32(&w, (uint32_t)now);
            node_send_id(n, s->id, &s->addr, buf, w.len);
        }
        p->next_ping = now + c->ping_cycle_ms / (n->nsn > 0 ? n->nsn : 1);
    }
    if (now - p->last_metrics >= c->metrics_every_ms) {
        send_metrics(n);
        p->last_metrics = now;
    }
}

/* ---------------------------------------------------------------- recepción */

static void read_ref(rbuf_t* r, ref_t* ref) {
    ref->id = r_u16(r);
    addr_read(r, &ref->addr);
}

static void on_batch(smnet_node_t* n, rbuf_t* r, double now) {
    peer_t* p = &n->peer;
    int cnt = r_u8(r);
    for (int i = 0; i < cnt && !r->err; i++) {
        uint16_t id = r_u16(r);
        uint8_t seq = r_u8(r);
        const uint8_t* raw = r->p + r->pos;
        smnet_state_t st;
        state_read(r, &st);
        if (r->err) break;
        if (id >= SMNET_MAX_PLAYERS || id == n->id) continue;
        proof_note_received(n, id, seq, raw);
        direct_note(n, id, seq, now, 1);
        peer_accept_remote(n, id, seq, &st, now);
    }
    (void)p;
}

/* Estado de un remoto (por SN o por atajo): se entrega si es más nuevo que lo que ya había. */
int peer_accept_remote(smnet_node_t* n, uint16_t id, uint8_t seq, const smnet_state_t* st, double now) {
    if (id >= SMNET_MAX_PLAYERS || id == n->id) return 0;
    remote_t* rm = &n->peer.remotes[id];
    if (rm->valid && now - rm->last_rx_ms < 1000 && (uint8_t)(seq - rm->seq) > 128) return 0;
    if (rm->valid && seq == rm->seq && now - rm->last_rx_ms < 1000) return 0;   /* duplicado (dos SN o SN + directo) */
    rm->valid = 1;
    rm->seq = seq;
    rm->st = *st;
    rm->last_rx_ms = now;
    n->stats.entity_updates++;
    if (n->on_entity) n->on_entity(n->user, id, seq, st, now);
    return 1;
}

static void on_full(smnet_node_t* n, rbuf_t* r, double now) {
    int cnt = r_u8(r);
    uint8_t blob[SMNET_MAX_BLOB];
    for (int i = 0; i < cnt && !r->err; i++) {
        uint16_t id = r_u16(r);
        uint8_t seq = r_u8(r);
        uint16_t len = r_u16(r);
        if (len > SMNET_MAX_BLOB) { r->err = 1; break; }
        r_bytes(r, blob, len);
        if (r->err) break;
        n->stats.blob_updates++;
        if (n->on_blob && id != n->id) n->on_blob(n->user, id, seq, blob, len, now);
    }
}

/* Llegó algo de un SN: si es el alternativo del failover en paralelo y el primario todavía no
 * respondió, el alternativo pasa a ser el primario; si respondió el primario, se suelta al otro. */
static void switch_primary(smnet_node_t* n, const ref_t* to, double now) {
    peer_t* p = &n->peer;
    if (p->primary.id != SMNET_NO_ID && p->primary.id != to->id) send_simple(n, MSG_LEAVE, 0, &p->primary);
    p->primary = *to;
    p->last_rx_primary = now;
    p->primary_ok = 1;
    p->alt = NOREF;
    p->next = NOREF;
}

static void sn_heard(smnet_node_t* n, uint16_t sender, double now) {
    peer_t* p = &n->peer;
    if (p->next.id != SMNET_NO_ID && sender == p->next.id) { ref_t nx = p->next; switch_primary(n, &nx, now); return; }
    if (sender == p->primary.id) {
        p->last_rx_primary = now;
        if (!p->primary_ok) {
            p->primary_ok = 1;
            if (p->alt.id != SMNET_NO_ID && now < p->alt_until) send_simple(n, MSG_LEAVE, 0, &p->alt);
            p->alt = NOREF;
        }
        return;
    }
    if (p->alt.id != SMNET_NO_ID && sender == p->alt.id && now < p->alt_until && now >= p->alt_after && !p->primary_ok) {
        ref_t dead = p->primary;
        p->primary = p->alt;
        p->alt = NOREF;
        p->primary_ok = 1;
        p->last_rx_primary = now;
        send_failover_notice(n, dead.id, p->primary.id);
    }
}

int peer_receive(smnet_node_t* n, const smnet_addr_t* from, const smnet_hdr_t* h, rbuf_t* r, double now) {
    peer_t* p = &n->peer;
    switch (h->type) {
    case MSG_WELCOME: {
        uint32_t nonce = r_u32(r);
        uint16_t id = r_u16(r);
        uint32_t epoch = r_u32(r);
        ref_t pri, bak;
        read_ref(r, &pri);
        read_ref(r, &bak);
        if (r->err || nonce != n->cfg.nonce || id >= SMNET_MAX_PLAYERS || epoch < n->epoch_seen) return 0;
        {
            /* dirección observada, clave de la partida y nuestro certificado */
            smnet_addr_t pub;
            uint8_t gpk[32], cert[CERT_SIZE];
            addr_read(r, &pub);
            r_bytes(r, gpk, 32);
            r_bytes(r, cert, CERT_SIZE);
            if (r->err) return 0;
            if (n->have_game && memcmp(gpk, n->game_pk, 32) != 0) { n->stats.auth_rejected++; return 0; }
            if (!n->have_game) { memcpy(n->game_pk, gpk, 32); n->have_game = 1; }
            const uint16_t cid = (uint16_t)(cert[0] | (cert[1] << 8));
            if (cid != id || memcmp(cert + 2, link_identity(n->link), 32) != 0 || auth_check_cert(n, cert) != 0) { n->stats.auth_rejected++; return 0; }
            memcpy(n->cert, cert, CERT_SIZE);
            n->have_cert = 1;
            /* el enlace la manda primera en cada sesión; si cambió (volvimos a entrar con otro id),
               también por las abiertas, que nos tienen con el viejo */
            uint8_t pb[SMNET_HDR_SIZE + 32 + CERT_SIZE];
            int pl = auth_build_present(n, pb, sizeof pb);
            if (pl) link_set_presentation(n->link, pb, pl);
            n->public_addr = pub;
        }
        n->id = id;
        node_set_coord(n, h->sender, from, epoch);
        if (pri.id == SMNET_NO_ID) {      /* todavía no hay SN con lugar: reintentar más tarde */
            p->status = SMNET_PEER_JOINING;
            p->last_hello = now;
            return 1;
        }
        if (p->status == SMNET_PEER_CONNECTED && p->primary.id != SMNET_NO_ID) {
            /* re-presentación (coordinador nuevo): mismo traspaso ordenado que con ASSIGN */
            if (pri.id != p->primary.id && pri.id != p->next.id) {
                if (node_has_link(n, &pri.addr)) switch_primary(n, &pri, now);
                else { p->next = pri; p->next_until = now + 3000; }
            }
            if (bak.id != p->backup.id) { p->backup = bak; p->last_rx_backup = now; p->last_hb_backup = 0; }
            return 1;
        }
        p->primary = pri;
        p->backup = bak;
        p->primary_ok = 1;
        p->alt = NOREF;
        p->next = NOREF;
        p->last_rx_primary = p->last_rx_backup = now;
        if (p->status != SMNET_PEER_CONNECTED) {
            p->status = SMNET_PEER_CONNECTED;
            p->connected_ms = now;
            p->last_metrics = now - n->cfg.metrics_every_ms + n->cfg.first_metrics_ms;
            p->next_tick = now;
        }
        return 1;
    }
    case MSG_ASSIGN: {
        uint32_t epoch = r_u32(r);
        ref_t pri, bak;
        read_ref(r, &pri);
        read_ref(r, &bak);
        if (r->err || epoch < n->epoch_seen) return 0;
        if (pri.id != p->primary.id && pri.id != SMNET_NO_ID && pri.id != p->next.id) {
            /* traspaso ordenado. Si ya hay sesión con el nuevo (lo venimos midiendo), se pasa de
               inmediato; si no (NAT cono abriéndose, handshake), primero se conecta al nuevo y
               recién cuando responde se suelta al viejo. */
            if (p->primary.id == SMNET_NO_ID || node_has_link(n, &pri.addr)) {
                switch_primary(n, &pri, now);
                send_state(n);
            } else {
                p->next = pri;
                p->next_until = now + 3000;
            }
        }
        if (bak.id != p->backup.id) {
            p->backup = bak;
            p->last_rx_backup = now;
            p->last_hb_backup = 0;
        }
        return 1;
    }
    case MSG_PROMOTE: {
        uint32_t epoch = r_u32(r);
        if (r->err || epoch < n->epoch_seen) return 0;
        node_become_sn(n, now);
        return 1;
    }
    case MSG_DEMOTE: {
        uint32_t epoch = r_u32(r);
        if (r->err || epoch < n->epoch_seen) return 0;
        node_stop_sn(n);
        return 1;
    }
    case MSG_SN_LIST: {
        uint32_t epoch = r_u32(r);
        uint16_t cid = r_u16(r);
        smnet_addr_t caddr;
        addr_read(r, &caddr);
        int cnt = r_u8(r);
        sninfo_t list[SMNET_MAX_SN];
        if (cnt > SMNET_MAX_SN) cnt = SMNET_MAX_SN;
        for (int i = 0; i < cnt && !r->err; i++) {
            list[i].id = r_u16(r);
            addr_read(r, &list[i].addr);
            list[i].flags = r_u8(r);
        }
        if (r->err || epoch < n->epoch_seen) return 0;
        {   /* degradados (v1.4; opcional) */
            int nb = r_u8(r);
            for (int i = 0; i < nb && !r->err && i < BANLIST_MAX; i++) { uint16_t b = r_u16(r); if (!r->err) node_ban_local(n, b); }
            r->err = 0;
        }
        /* si viene directo del coordinador, su dirección real es el origen (la configurada puede
           no ser la pública) */
        node_set_coord(n, cid, h->sender == cid ? from : &caddr, epoch);
        node_set_snlist(n, list, cnt);
        return 1;
    }
    case MSG_PROOF_FWD:
        if (h->sender != p->primary.id && h->sender != p->next.id) return 0;
        return proof_on_forward(n, r, now);
    case MSG_REDIRECT: {
        /* "el coordinador es este": volver a presentarse directo a él */
        uint16_t cid = r_u16(r);
        smnet_addr_t ca;
        addr_read(r, &ca);
        if (r->err || p->status != SMNET_PEER_JOINING || n->have_coord) return 0;
        (void)cid;
        n->bootstrap = ca;
        send_hello(n, now);
        return 1;
    }
    case MSG_NEW_COORD: {
        uint32_t epoch = r_u32(r);
        if (r->err || epoch <= n->epoch_seen) return 0;
        node_set_coord(n, h->sender, from, epoch);
        /* volver a presentarse: el coordinador nuevo confirma el id (mismo nonce) o asigna
           uno nuevo si el nuestro quedó libre mientras no estábamos */
        send_hello(n, now);
        return 1;
    }
    case MSG_STATE_BATCH:
        if (p->status != SMNET_PEER_CONNECTED) return 0;
        if (node_is_banned(n, h->sender)) return 0;   /* degradado: lo suyo no se usa más */
        sn_heard(n, h->sender, now);
        p->last_data_ms = now;
        on_batch(n, r, now);
        return 1;
    case MSG_STATE_FULL:
        if (p->status != SMNET_PEER_CONNECTED) return 0;
        if (node_is_banned(n, h->sender)) return 0;
        sn_heard(n, h->sender, now);
        on_full(n, r, now);
        return 1;
    case MSG_HEARTBEAT:
        sn_heard(n, h->sender, now);
        if (h->sender == p->backup.id) p->last_rx_backup = now;
        return 1;
    case MSG_PONG: {
        uint32_t t = r_u32(r);
        if (r->err || h->sender >= SMNET_MAX_PLAYERS) return 0;
        uint32_t rtt = (uint32_t)now - t;
        if (rtt > 5000) return 0;
        if (rtt == 0) rtt = 1;
        uint16_t old = p->rtt[h->sender];
        p->rtt[h->sender] = old ? (uint16_t)((old * 3 + rtt) / 4) : (uint16_t)rtt;
        return 1;
    }
    }
    return 0;
}
