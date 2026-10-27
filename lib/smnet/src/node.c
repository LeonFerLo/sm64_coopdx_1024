/* Nodo: contenedor de los tres roles, enrutamiento de mensajes y entrega local. */
#include <stdlib.h>
#include <string.h>
#include "node_internal.h"
#include "../third_party/monocypher/monocypher.h"

void smnet_node_cfg_default(smnet_node_cfg_t* c) {
    memset(c, 0, sizeof *c);
    c->nonce = 1;
    c->upload_kbps = 10000;
    c->nat = SMNET_NAT_CONE;
    c->hello_retry_ms = 500;
    c->primary_timeout_ms = 1100;   /* con 750, un SN cargado (cientos de bots en una PC) perdía a sus peers en masa */
    c->backup_dead_ms = 3000;
    c->backup_hb_ms = 1000;
    c->metrics_every_ms = 10000;
    c->first_metrics_ms = 1500;
    c->ping_cycle_ms = 5000;
    c->remote_expire_ms = 3000;
    c->encrypt = 1;
    c->verify_neighbors = 1;
    c->requested_id = SMNET_NO_ID;
    c->direct = 1;
    c->direct_max = 8;
    c->direct_upload_frac = 0.3f;
    c->direct_eval_ms = 5000;

    smnet_sn_cfg_t* s = &c->sn;
    s->down_budget_kbps = 600;
    s->full_budget_kbps = 160;
    s->tier_k[0] = 32; s->tier_k[1] = 128;
    s->tier_hz[0] = 30; s->tier_hz[1] = 10; s->tier_hz[2] = 2;
    s->full_k = 4;
    s->full_radius = 1500.f;
    s->rank_every_ticks = 10;
    s->max_datagrams_per_tick = 3;
    s->relay_base_hz = 2.f;
    s->subscribe_every_ms = 250;
    s->subscribe_ttl_ms = 3000;
    s->hosted_expire_ms = 1000;
    s->remote_stale_ms = 2500;
    s->report_every_ms = 1000;

    smnet_coord_cfg_t* k = &c->coord;
    k->rebalance_ms = 10000;
    k->ondemand_gap_ms = 1000;
    k->sn_min_upload_kbps = 20000;
    k->sn_min_uptime_ms = 30000;
    k->sn_usable = 0.6f;
    k->margin = 1.25f;
    k->demote_margin = 1.7f;
    k->max_fanout = 48;
    k->hysteresis_ms = 15.f;
    k->beta_ms = 10.f;
    k->max_move_frac = 0.05f;
    k->unknown_rtt_ms = 250.f;
    k->emergency_overflow = 1.25f;
    k->sn_dead_ms = 6000;      /* margen para unos segundos sin CPU (con 3000/6000, abrir OBS con
                                  cientos de bots en la misma PC los daba por muertos en masa) */
    k->peer_dead_ms = 15000;
    k->replicas = 2;
    k->hb_every_ms = 250;
    k->takeover_ms = 2000;     /* con 750, una red ocupada (el host con 1000 jugadores) o un cuadro
                                  largo de la réplica partían la partida en dos */
    k->sync_every_ms = 2000;
    k->first_rebalance_ms = 2000;
}

static void dispatch(smnet_node_t* n, const smnet_addr_t* from, const uint8_t* data, int len, double now, int local);

/* La capa de enlace saca datagramas por acá ... */
static void link_out(void* ctx, const smnet_addr_t* to, const uint8_t* d, int len) {
    smnet_node_t* n = ctx;
    n->stats.tx_bytes += (uint64_t)len;
    n->stats.tx_packets++;
    n->send(n->user, to, d, len);
}
/* ... y entrega los mensajes ya descifrados por acá. */
static void link_in(void* ctx, const smnet_addr_t* from, const uint8_t* msg, int len) {
    smnet_node_t* n = ctx;
    dispatch(n, from, msg, len, n->now, 0);
}

/* Control por el canal confiable; estado y mensajes periódicos (que ya se repiten), no. */
static int msg_reliable(uint8_t type) {
    switch (type) {
    case MSG_LINK_OFFER: case MSG_LINK_ANSWER: case MSG_LINK_CLOSE:
    case MSG_WELCOME: case MSG_ASSIGN: case MSG_PROMOTE: case MSG_DEMOTE: case MSG_SN_LIST:
    case MSG_NEW_COORD: case MSG_FAILOVER: case MSG_LEAVE:
    case MSG_INTRO: case MSG_INTRO_REQ: case MSG_PRESENT: case MSG_GAP_REPORT:
    case MSG_APP_UP: case MSG_APP_DOWN:
    case MSG_PLAYER_INFO: case MSG_ROSTER: case MSG_AREA_OWNER: case MSG_AREA_NOTICE:
        return 1;
    default:
        return 0;
    }
}

smnet_node_t* smnet_node_new(const smnet_node_cfg_t* cfg, smnet_send_fn send, void* user) {
    smnet_node_t* n = calloc(1, sizeof *n);
    if (!n) return NULL;
    if (cfg) n->cfg = *cfg; else smnet_node_cfg_default(&n->cfg);
    if (n->cfg.sn.full_k > SMNET_MAX_FULL) n->cfg.sn.full_k = SMNET_MAX_FULL;   /* vw->full[] */
    if (n->cfg.sn.full_k < 0) n->cfg.sn.full_k = 0;
    n->send = send;
    n->user = user;
    n->owner_area = n->area_owner = SMNET_NO_ID;
    n->id = SMNET_NO_ID;
    n->coord_id = SMNET_NO_ID;
    peer_init(n);
    uint8_t seed[32], zero[32] = { 0 };
    if (memcmp(n->cfg.seed, zero, 32) != 0) memcpy(seed, n->cfg.seed, 32);
    else if (n->cfg.encrypt) {
        /* con cifrado, la semilla es obligatoria: derivarla de algo conocido haría previsibles
           todas las claves */
        peer_free(n); free(n); return NULL;
    } else { uint8_t nb[4] = { (uint8_t)n->cfg.nonce, (uint8_t)(n->cfg.nonce >> 8), (uint8_t)(n->cfg.nonce >> 16), (uint8_t)(n->cfg.nonce >> 24) };
           crypto_blake2b(seed, 32, nb, 4); }
    link_cfg_t lc;
    link_cfg_default(&lc);
    lc.encrypt = n->cfg.encrypt;
    lc.wall_offset_ms = n->cfg.wall_offset_ms;
    memcpy(lc.identity_seed, n->cfg.identity_seed, 32);
    crypto_wipe(n->cfg.identity_seed, 32);
    n->link = link_new(&lc, seed, link_out, link_in, n);
    crypto_wipe(seed, 32);
    if (!n->peer.remotes || !n->peer.rtt || !n->link) { smnet_node_free(n); return NULL; }
    if (memcmp(n->cfg.game_pk, zero, 32) != 0) { memcpy(n->game_pk, n->cfg.game_pk, 32); n->have_game = 1; }
    return n;
}

void smnet_node_free(smnet_node_t* n) {
    if (!n) return;
    peer_free(n);
    if (n->sn) sn_free(n->sn);
    if (n->co) coord_free(n->co);
    link_free(n->link);
    app_free(n);
    free(n->loop);
    free(n);
}

void smnet_node_set_callbacks(smnet_node_t* n, smnet_on_entity_fn on_entity, smnet_on_blob_fn on_blob) {
    n->on_entity = on_entity;
    n->on_blob = on_blob;
}

/* ------------------------------------------------------------- envío */

static void loop_push(smnet_node_t* n, const uint8_t* d, int len) {
    if (len > SMNET_MTU + 64) return;
    if (n->loop_n == n->loop_cap) {
        int nc = n->loop_cap ? n->loop_cap * 2 : 64;
        void* p = realloc(n->loop, sizeof(loopmsg_t) * (size_t)nc);
        if (!p) return;
        n->loop = p;
        n->loop_cap = nc;
    }
    n->loop[n->loop_n].len = len;
    memcpy(n->loop[n->loop_n].d, d, (size_t)len);
    n->loop_n++;
}

static int is_self_addr(const smnet_node_t* n, const smnet_addr_t* a) {
    return n->cfg.self_addr.len > 0 && smnet_addr_eq(a, &n->cfg.self_addr);
}

void node_send_addr(smnet_node_t* n, const smnet_addr_t* to, const uint8_t* d, int len) {
    if (is_self_addr(n, to)) { loop_push(n, d, len); return; }
    if (to->len == 0 || len < SMNET_HDR_SIZE) return;
    /* la presentación (PRESENT) la pone el enlace primera en cada sesión: link_set_presentation */
    if (n->sn) {
        n->sn->tx_bytes_window += (uint64_t)(len + LINK_OVERHEAD);
        uint8_t t = d[1];
        if (t == MSG_STATE_BATCH || t == MSG_STATE_FULL || t == MSG_HEARTBEAT) n->sn->peer_bytes_window += (uint64_t)(len + LINK_OVERHEAD);
    }
    link_send(n->link, to, d, len, msg_reliable(d[1]), n->now);
}

void node_ban_local(smnet_node_t* n, uint16_t id) {
    if (node_is_banned(n, id)) return;
    if (n->nbanned == BANLIST_MAX) { memmove(n->banned, n->banned + 1, sizeof(uint16_t) * (BANLIST_MAX - 1)); n->nbanned--; }
    n->banned[n->nbanned++] = id;
}
int node_is_banned(const smnet_node_t* n, uint16_t id) {
    for (int i = 0; i < n->nbanned; i++) if (n->banned[i] == id) return 1;
    return 0;
}

void node_punch(smnet_node_t* n, const smnet_addr_t* to) {
    if (to->len == 0 || is_self_addr(n, to)) return;
    link_punch(n->link, to, n->now);
}

int node_has_link(const smnet_node_t* n, const smnet_addr_t* to) { return link_established(n->link, to); }

void node_send_intro(smnet_node_t* n, uint16_t to_id, const smnet_addr_t* to, uint16_t who, const smnet_addr_t* who_addr) {
    uint8_t buf[64];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    node_hdr(n, &w, MSG_INTRO, 0, 0);
    w_u16(&w, who);
    addr_write(&w, who_addr);
    if (!w.err) node_send_id(n, to_id, to, buf, w.len);
}

void node_send_id(smnet_node_t* n, uint16_t id, const smnet_addr_t* to, const uint8_t* d, int len) {
    if (id != SMNET_NO_ID && id == n->id) { loop_push(n, d, len); return; }
    node_send_addr(n, to, d, len);
}

void node_send_coord(smnet_node_t* n, const uint8_t* d, int len) {
    if (n->co && n->co->role == SMNET_COORD_ACTIVE) { loop_push(n, d, len); return; }
    if (n->have_coord) node_send_id(n, n->coord_id, &n->coord_addr, d, len);
    else if (n->bootstrap.len) node_send_addr(n, &n->bootstrap, d, len);
}

void node_hdr(smnet_node_t* n, wbuf_t* w, uint8_t type, uint8_t flags, uint16_t seq) {
    smnet_hdr_t h = { SMNET_PROTO_VERSION, flags, type, n->id, seq, n->peer.tick };
    hdr_write(w, &h);
}

/* --------------------------------------------------------- roles */

void node_become_sn(smnet_node_t* n, double now) {
    if (n->sn) return;
    n->sn = sn_new(n, now);
    if (n->sn) sn_sync_others(n);
}

void node_stop_sn(smnet_node_t* n) {
    if (!n->sn) return;
    sn_free(n->sn);
    n->sn = NULL;
}

/* SN de la lista vigente, o que salió hace menos de 8 s: con cambios seguidos de SN (multitudes),
   la lista de uno va atrás y se descartaban los reenvíos de un SN recién bajado (todos los
   impostores de su zona desaparecían unos segundos) */
int node_is_sn_or_recent(const smnet_node_t* n, uint16_t id) {
    for (int i = 0; i < n->nsn; i++) if (n->snlist[i].id == id) return 1;
    return id < SMNET_MAX_PLAYERS && n->sn_gone_ms[id] > 0 && n->now - n->sn_gone_ms[id] < 8000.0;
}

void node_set_snlist(smnet_node_t* n, const sninfo_t* list, int cnt) {
    if (cnt > SMNET_MAX_SN) cnt = SMNET_MAX_SN;
    for (int i = 0; i < n->nsn; i++) {
        uint16_t id = n->snlist[i].id; int still = 0;
        for (int j = 0; j < cnt; j++) if (list[j].id == id) { still = 1; break; }
        if (!still && id < SMNET_MAX_PLAYERS) n->sn_gone_ms[id] = n->now > 0 ? n->now : 1;
    }
    for (int j = 0; j < cnt; j++) if (list[j].id < SMNET_MAX_PLAYERS) n->sn_gone_ms[list[j].id] = 0;
    memcpy(n->snlist, list, sizeof(sninfo_t) * (size_t)cnt);
    n->nsn = cnt;
    if (n->sn) {
        sn_sync_others(n);
        /* los SN se perforan entre sí (todos tienen NAT abierto o cono: la dirección vale para todos) */
        for (int i = 0; i < cnt; i++)
            if (list[i].id != n->id && !node_has_link(n, &list[i].addr)) node_punch(n, &list[i].addr);
    }
}

void node_set_coord(smnet_node_t* n, uint16_t id, const smnet_addr_t* addr, uint32_t epoch) {
    if (epoch < n->epoch_seen) return;
    n->epoch_seen = epoch;
    n->coord_id = id;
    n->coord_addr = *addr;
    n->have_coord = 1;
}

void smnet_node_start(smnet_node_t* n, const smnet_addr_t* coordinator, double now) {
    n->now = now;
    if (coordinator) n->bootstrap = *coordinator;
    if (n->cfg.is_host) coord_start_active(n, now);
    n->peer.status = SMNET_PEER_JOINING;
    n->peer.last_hello = now - n->cfg.hello_retry_ms;   /* HELLO en el próximo update */
}

void smnet_node_set_local(smnet_node_t* n, uint16_t area, const smnet_state_t* st, const uint8_t* blob, int blob_len) {
    peer_t* p = &n->peer;
    p->area = area;
    p->local = *st;
    if (blob && blob_len > 0) {
        if (blob_len > SMNET_MAX_BLOB) blob_len = SMNET_MAX_BLOB;
        memcpy(p->blob, blob, (size_t)blob_len);
        p->blob_len = blob_len;
        p->blob_pending = 1;
    }
}

/* ------------------------------------------------------------ recepción */

static void dispatch(smnet_node_t* n, const smnet_addr_t* from, const uint8_t* data, int len, double now, int local) {
    rbuf_t r = { data, len, 0, 0 };
    smnet_hdr_t h;
    if (hdr_read(&r, &h) != 0) { n->stats.bad_packets++; return; }
    /* autorización: el remitente de la cabecera tiene que ser el id verificado de la sesión */
    if (!local && !auth_allowed(n, from, &h)) { n->stats.unauthorized++; return; }
    if (!local && h.type == MSG_NEW_COORD && (node_is_banned(n, h.sender) || auth_check_new_coord(n, from, &h, &r) != 0)) { n->stats.unauthorized++; return; }
    /* réplica a punto de tomar el control: el coordinador contestó, entonces sigue vivo */
    if (!local && n->co && n->co->probe_at > 0 && h.sender == n->co->coord_id) n->co->probe_heard = 1;
    int handled = 0;
    switch (h.type) {
    case MSG_PRESENT:
        handled = auth_on_present(n, from, &r);
        break;
    case MSG_STATE_PROOF: case MSG_RELAY_PROOF:
        if (n->sn) handled = sn_on_proof(n, &h, &r, h.type == MSG_RELAY_PROOF);
        break;
    case MSG_VERIFY_SET:
        if (n->sn) handled = sn_on_verify_set(n, &h, &r);
        break;
    case MSG_GAP_REPORT:
        if (n->co && n->co->role == SMNET_COORD_ACTIVE) handled = coord_receive(n, from, &h, &r, now);
        break;
    case MSG_HELLO:
        if (n->co && n->co->role == SMNET_COORD_ACTIVE) handled = coord_receive(n, from, &h, &r, now);
        else if (n->have_coord) {
            /* no soy el coordinador: le digo quién es (el que entra le habla directo, así el
               coordinador ve su identidad en el handshake) */
            uint8_t buf[64];
            wbuf_t w = { buf, 0, sizeof buf, 0 };
            node_hdr(n, &w, MSG_REDIRECT, 0, 0);
            w_u16(&w, n->coord_id);
            addr_write(&w, &n->coord_addr);
            if (!w.err) node_send_addr(n, from, buf, w.len);
            handled = 1;
        }
        break;
    case MSG_METRICS: case MSG_FAILOVER: case MSG_SN_REPORT:
    case MSG_COORD_SYNC: case MSG_COORD_HB:
        if (n->co) handled = coord_receive(n, from, &h, &r, now);
        else if (h.type == MSG_COORD_SYNC || h.type == MSG_COORD_HB) {
            /* me eligieron réplica */
            n->co = coord_new();
            if (n->co) { n->co->role = SMNET_COORD_REPLICA; handled = coord_receive(n, from, &h, &r, now); }
        }
        break;
    case MSG_NEW_COORD: {
        /* lo procesan dos roles: cada uno con su propio cursor */
        rbuf_t r2 = r;
        if (n->co) coord_receive(n, from, &h, &r2, now);
        handled = peer_receive(n, from, &h, &r, now);
        break;
    }
    case MSG_INTRO: {
        /* abrir el filtro de nuestro NAT hacia esa dirección (el otro está intentando entrar) */
        uint16_t who = r_u16(&r);
        smnet_addr_t a;
        addr_read(&r, &a);
        (void)who;
        if (!r.err) node_punch(n, &a);
        handled = !r.err;
        break;
    }
    case MSG_INTRO_REQ: case MSG_DIRECT_SET:
        if (n->sn) handled = sn_receive(n, from, &h, &r, now);
        break;
    case MSG_LINK_OFFER: case MSG_LINK_ANSWER: {
        /* etapa 0 y 1: las reenvía el SN; etapa 2: es para el peer */
        uint8_t stage = data[SMNET_HDR_SIZE < len ? SMNET_HDR_SIZE : 0];
        if (stage < 2) { if (n->sn) handled = sn_forward_link(n, from, &h, &r); }
        else handled = direct_receive(n, from, &h, &r, now);
        break;
    }
    case MSG_LINK_CLOSE: case MSG_STATE_DIRECT:
        handled = direct_receive(n, from, &h, &r, now);
        break;
    case MSG_STATE_UP: case MSG_PING: case MSG_LEAVE: case MSG_STATE_RELAY:
    case MSG_SUBSCRIBE: case MSG_RELAY_FULL:
        if (n->sn) handled = sn_receive(n, from, &h, &r, now);
        break;
    case MSG_APP_UP:
        handled = app_on_up(n, &h, &r, local, now);
        break;
    case MSG_APP_DOWN:
        handled = app_on_down(n, &h, &r, local, now);
        break;
    case MSG_PLAYER_INFO: case MSG_AREA_NOTICE:
        if (n->co) handled = roster_coord_receive(n, &h, &r);
        break;
    case MSG_ROSTER: case MSG_AREA_OWNER:
        handled = roster_receive(n, &h, &r, local);
        break;
    case MSG_HEARTBEAT:
        if (h.flags & HB_FROM_SN) handled = peer_receive(n, from, &h, &r, now);
        else if (n->sn) handled = sn_receive(n, from, &h, &r, now);
        break;
    default:
        handled = peer_receive(n, from, &h, &r, now);
    }
    if (!handled || r.err) n->stats.bad_packets += r.err ? 1 : 0;
}

void smnet_node_receive(smnet_node_t* n, const smnet_addr_t* from, const uint8_t* data, int len, double now) {
    n->now = now;
    n->stats.rx_bytes += (uint64_t)len;
    n->stats.rx_packets++;
    link_receive(n->link, from, data, len, now);
}

void smnet_node_update(smnet_node_t* n, double now) {
    n->now = now;
    /* entrega local: solo lo que había al empezar (evita ciclos) */
    if (n->loop_n) {
        int cnt = n->loop_n;
        loopmsg_t* tmp = malloc(sizeof(loopmsg_t) * (size_t)cnt);
        if (tmp) {
            memcpy(tmp, n->loop, sizeof(loopmsg_t) * (size_t)cnt);
            memmove(n->loop, n->loop + cnt, sizeof(loopmsg_t) * (size_t)(n->loop_n - cnt));
            n->loop_n -= cnt;
            for (int i = 0; i < cnt; i++) dispatch(n, &n->cfg.self_addr, tmp[i].d, tmp[i].len, now, 1);
            free(tmp);
        }
    }
    peer_update(n, now);
    if (n->sn) sn_update(n, now);
    if (n->co) coord_update(n, now);
    if (n->co) roster_coord_update(n, now);
    roster_peer_update(n);
    app_flush(n);
    link_update(n->link, now);
}

void smnet_node_info(const smnet_node_t* n, smnet_node_info_t* o) {
    memset(o, 0, sizeof *o);
    o->status = n->peer.status;
    o->id = n->id;
    o->primary = n->peer.primary.id;
    o->backup = n->peer.backup.id;
    o->coord_id = n->coord_id;
    o->epoch = n->epoch_seen;
    o->is_sn = n->sn != NULL;
    o->coord_role = n->co ? n->co->role : SMNET_COORD_NONE;
    o->hosted = n->sn ? n->sn->hosted_cached : 0;
    o->n_sn = n->nsn;
    o->failovers = n->peer.failovers;
    o->rejoins = n->peer.rejoins;
    o->last_data_ms = n->peer.last_data_ms;
    o->last_failover_ms = n->peer.last_failover_ms;
    o->last_failover_kind = n->peer.last_failover_kind;
    const direct_t* d = &n->peer.dir;
    for (int i = 0; i < d->nl; i++) if (d->l[i].state == DL_UP && o->n_direct < 16) o->direct_ids[o->n_direct++] = d->l[i].id;
    o->direct_opens = d->opens; o->direct_attempts = d->attempts; o->direct_failed = d->failed;
    o->direct_nogain = d->closed_nogain; o->direct_lost = d->lost;
    o->direct_f_noanswer = d->f_noanswer; o->direct_f_rejected = d->f_rejected; o->direct_f_nat = d->f_nat; o->direct_f_timeout = d->f_timeout;
    o->direct_life_mean_s = d->life_n ? d->life_sum / d->life_n / 1000.0 : 0;
}

uint8_t smnet_node_seq(const smnet_node_t* n) { return (uint8_t)n->peer.seq; }

void smnet_node_set_evil(smnet_node_t* n, float drop, int alter, int drop_proofs) {
    n->cfg.sn.evil_drop = drop;
    n->cfg.sn.evil_alter = alter;
    n->cfg.sn.evil_drop_proofs = drop_proofs;
}
const smnet_stats_t* smnet_node_stats(const smnet_node_t* n) {
    /* lo que cuenta la verificación (proof.c) vive en peer.ver; se copia acá */
    smnet_stats_t* st = (smnet_stats_t*)&n->stats;
    const verify_t* v = &n->peer.ver;
    st->proofs_ok = v->proofs_ok; st->proofs_bad = v->proofs_bad;
    st->altered_detected = v->altered; st->gap_reports = v->reports_sent;
    st->ver_emitted = v->tot_emitted; st->ver_got = v->tot_got;
    return st;
}
const smnet_link_stats_t* smnet_node_link_stats(const smnet_node_t* n) { return link_stats(n->link); }

void smnet_node_leave(smnet_node_t* n, double now) {
    (void)now;
    if (n->peer.primary.id == SMNET_NO_ID) return;
    uint8_t buf[SMNET_HDR_SIZE];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    node_hdr(n, &w, MSG_LEAVE, 0, 0);
    node_send_id(n, n->peer.primary.id, &n->peer.primary.addr, buf, w.len);
}
