/* Estructuras internas del nodo (peer + SN + coordinador). */
#ifndef SMNET_NODE_INTERNAL_H
#define SMNET_NODE_INTERNAL_H

#include "smnet.h"
#include "wire.h"
#include "link.h"

#define TICK_MS (1000.0 / 30.0)
#define CERT_SIZE 98
#define BANLIST_MAX 32
#define PROOF_HASH 6           /* bytes de hash por estado (segunda preimagen: 2^48 intentos) */
#define PROOF_MAX  32          /* estados por prueba */
#define VER_RING   64
#define VER_TRACK  8           /* vecinos verificados por cada viewer */
enum { GAP_DROP = 1, GAP_ALTERED = 2, GAP_NOPROOF = 3 };

/* ------------------------------------------------------- atajo directo */
#define DIRECT_MAX   16
#define GAIN_RING    32
enum { DL_OFFERED = 1, DL_OPENING, DL_UP };

typedef struct {
    uint16_t     id;
    uint8_t      state, nat, mine;      /* mine: lo ofrecimos nosotros */
    smnet_addr_t addr;
    uint32_t     token;
    double       t_start, t_up, last_rx;
    float        gain_ms;               /* EWMA de (llegada por SN - llegada directa) */
    int          gain_n;
    uint8_t      g_seq[GAIN_RING];
    double       g_td[GAIN_RING], g_ts[GAIN_RING];
} dlink_t;

typedef struct {
    dlink_t   l[DIRECT_MAX];
    int       nl;
    double    tokens, last_refill, next_eval, next_set;
    double*   near_since;               /* [SMNET_MAX_PLAYERS] 0 = no está entre los 32 más cercanos */
    double*   retry_after;              /* [SMNET_MAX_PLAYERS] backoff por par */
    uint8_t*  fails;                    /* [SMNET_MAX_PLAYERS] */
    uint8_t*  rank;                     /* [SMNET_MAX_PLAYERS] puesto por cercanía (255 = lejos) */
    struct dd { float d; uint16_t id; } *tmp;
    uint32_t  opens, attempts, failed, closed_nogain, lost, accepted, rejected;
    uint32_t  f_noanswer, f_rejected, f_nat, f_timeout;   /* motivos de falla (lado que ofrece) */
    double    life_sum;
    uint32_t  life_n;
} direct_t;

/* ----------------------------------------------- verificación de estado */
/* Un estado de un vecino verificado: lo recibido y lo firmado se emparejan por seq, lleguen en el
 * orden que lleguen (la prueba suele llegar antes que los últimos estados que cubre). */
#define RH_RX   1
#define RH_PF   2
#define RH_DONE 4
typedef struct { uint8_t seq, fl; uint8_t h_rx[PROOF_HASH], h_pf[PROOF_HASH]; double t; } rh_t;
typedef struct {
    uint16_t id;
    double   since, last_proof;
    rh_t     ring[VER_RING];
    uint32_t emitted, got, mism, rx;
    uint8_t  have_last, last_first, last_cnt;
} vtrack_t;
typedef struct {
    uint8_t  sent_h[PROOF_MAX][PROOF_HASH];
    int      nsent;
    uint8_t  first_seq;
    double   next_proof, next_track, window_start, last_report, quiet_until;
    uint16_t watch_sn;
    vtrack_t tr[VER_TRACK];
    int      ntr;
    uint8_t* pk;                       /* [SMNET_MAX_PLAYERS * 32] claves de los vecinos (de sus certificados) */
    uint8_t* pk_ok;
    uint32_t proofs_ok, proofs_bad, altered, reports_sent;
    uint64_t tot_emitted, tot_got;
} verify_t;

/* ---------------------------------------------------------------- peer */
typedef struct { float tokens; double t; } app_bucket_t;
#define APP_RATE_ALL     4.0f
#define APP_BURST_ALL    16.0f
#define APP_RATE_AREA    60.0f
#define APP_BURST_AREA   120.0f
#define APP_RATE_DIRECT  30.0f
#define APP_BURST_DIRECT 60.0f
typedef struct { uint16_t id; smnet_addr_t addr; } ref_t;   /* id == SMNET_NO_ID: vacío */

typedef struct {
    uint8_t       valid, seq;
    smnet_state_t st;
    double        last_rx_ms;
} remote_t;

typedef struct {
    smnet_peer_status_t status;
    ref_t     primary, backup;
    ref_t     alt;                     /* failover en paralelo: segundo candidato a prueba */
    double    alt_until;
    double    alt_after;               /* no cambiar al alternativo antes de esto (le damos tiempo al backup) */
    int       primary_ok;              /* el primario actual ya respondió */
    ref_t     next;                    /* traspaso pendiente: primario nuevo que todavía no respondió */
    double    next_until;
    double    last_rx_primary, last_rx_backup, last_hb_backup;
    double    last_hello, last_metrics, connected_ms, last_data_ms;
    double    next_tick;
    uint16_t  seq, tick;
    uint16_t  area;
    smnet_state_t local;
    uint8_t   blob[SMNET_MAX_BLOB];
    int       blob_len, blob_pending;
    remote_t* remotes;                 /* [SMNET_MAX_PLAYERS] */
    uint16_t* rtt;                     /* [SMNET_MAX_PLAYERS] RTT medido al SN con ese id (0 = no medido) */
    int       ping_idx;
    double    next_ping;
    uint32_t  failovers, rejoins;
    double    last_failover_ms;
    int       last_failover_kind;      /* 1 backup, 2 salto al mejor SN, 3 reingreso */
    direct_t  dir;
    verify_t  ver;
} peer_t;

/* ------------------------------------------------------------------ SN */
typedef struct {
    uint16_t* cand;
    float*    cand_hz;
    int       ncand;
    float*    prio;                    /* [SMNET_MAX_PLAYERS] */
    uint8_t*  sent_seq;                /* [SMNET_MAX_PLAYERS] */
    uint8_t*  sent_valid;              /* [SMNET_MAX_PLAYERS] */
    uint16_t* blob_sent;               /* [SMNET_MAX_PLAYERS] */
    uint16_t  full[SMNET_MAX_FULL];
    int       nfull;
    uint16_t  out_seq;
    double    last_tx_ms;
    uint8_t   cert_sent[SMNET_MAX_PLAYERS / 8];   /* certificados ya reenviados a este viewer */
    uint16_t  vset[VER_TRACK];         /* vecinos que este viewer verifica */
    int       nvset;
    uint16_t  dset[DIRECT_MAX];        /* vecinos que este viewer recibe por atajo directo */
    int       ndset;
    double    dset_until;
} view_t;

typedef struct {
    uint8_t       known, hosted;
    uint8_t       arrived;             /* recién alojado: reenviarlo ya a todos los SN */
    uint8_t       has_cert, proof_first, proof_cnt;
    uint16_t      proof_ver, proof_fwd;
    double        proof_t;           /* cuándo llegó la última prueba (no se reenvían viejas) */
    uint8_t       cert[CERT_SIZE];
    uint8_t       proof_h[PROOF_MAX * PROOF_HASH];
    uint8_t       proof_sig[64];
    uint16_t      area, host_sn;
    uint8_t       seq8;
    smnet_state_t st;
    double        last_ms, last_up_ms;
    smnet_addr_t  addr;                /* si está alojado */
    uint8_t       blob[SMNET_MAX_BLOB];
    uint16_t      blob_len, blob_ver;
    view_t*       v;
    app_bucket_t  app_bucket[3];      /* todos, área, directo (en el primer SN) */
} ent_t;

typedef struct {
    uint16_t     id;
    smnet_addr_t addr;
    uint8_t      flags;
    uint8_t*     sub;                  /* [SMNET_MAX_PLAYERS] lo que ese SN nos pidió de nuestros alojados */
    double       sub_ms;
    float*       prio;                 /* [SMNET_MAX_PLAYERS] */
    uint8_t*     sent_seq;
    uint8_t*     sent_valid;
    uint16_t*    blob_sent;
    uint16_t     out_seq;
} other_t;

typedef struct {
    ent_t*    ents;                    /* [SMNET_MAX_PLAYERS] */
    other_t   others[SMNET_MAX_SN];
    int       nothers;
    uint16_t  tick;
    double    next_tick, last_report, last_subscribe, start_ms;
    int       dirty;
    uint8_t*  want;                    /* [SMNET_MAX_PLAYERS] tier máximo pedido por mis viewers */
    struct de { float d; uint16_t id; } *tmp;
    uint16_t* elig;
    uint64_t  tx_bytes_window;
    uint64_t  peer_bytes_window;       /* solo lo que va a peers alojados (lotes, rico, latidos) */
    uint32_t  peer_egress_kbps;
    double    window_start;
    uint32_t  egress_kbps;
    int       hosted_cached;           /* alojados (se recalcula por tick) */
    int       sub_now;                 /* mandar suscripciones en este tick */
} sn_t;

/* --------------------------------------------------------- coordinador */
typedef struct {
    uint8_t      used, is_sn;
    uint32_t     nonce;
    smnet_addr_t addr;
    uint32_t     upload_kbps;
    uint8_t      nat;
    uint16_t     area;
    uint16_t     primary, backup;
    uint16_t     cap;
    double       join_ms, last_seen_ms, last_report_ms, sn_since_ms, last_hosted_ms, freed_ms;
    uint32_t     egress_kbps;
    uint16_t     n_rtt;
    uint16_t     rtt_id[SMNET_MAX_SN];
    uint16_t     rtt_ms[SMNET_MAX_SN];
    uint8_t      dirty;                /* hay que mandarle ASSIGN */
    uint8_t      provisional;          /* asignado sin mediciones de RTT: moverlo no cuenta para el tope */
    uint8_t      got_report;           /* SN: ya mandó al menos un SN_REPORT desde que se lo promovió */
    uint8_t      promote_tries;
    uint8_t      suspects;             /* reportes de failover contra este SN en la ventana */
    double       suspect_ms, last_assign_ms, last_ctl_ms;
    uint16_t     sync_gen;             /* réplica: última sincronización completa que lo incluyó */
    uint8_t      pk[32];               /* identidad del nodo (la del enlace al entrar) */
    double       banned_until;         /* no elegible como SN hasta entonces (permanente: infinito) */
    app_bucket_t app_bucket[3];        /* mensajes de la aplicación que pasan por el coordinador */
    uint8_t      pinfo[SMNET_MAX_PINFO], pinfo_len, pinfo_dirty, announced, want_snapshot, owner_told;
    uint16_t     told_owner, told_owner_area;
} cnode_t;
#define COORD_MAX_AREAS 256

typedef struct {
    smnet_coord_role_t role;
    uint32_t  epoch;
    cnode_t*  nodes;                   /* [SMNET_MAX_PLAYERS] */
    uint16_t  replicas[4];
    int       nreplicas;
    int       my_rank;                 /* si soy réplica */
    double    last_hb_rx;              /* réplica: último COORD_HB */
    double    last_tick;               /* réplica: última vuelta de coord_update (para no culpar al coordinador de una pausa propia) */
    double    probe_at;                /* réplica sin latidos: cuándo se le preguntó al coordinador si sigue */
    uint8_t   probe_heard;             /* … y si contestó (entonces no se toma el control) */
    double    last_hb_tx, last_sync, last_rebalance, last_ondemand, next_rebalance;
    double    last_snlist_bcast, last_live;
    double    took_over_ms;            /* momento de la toma de control (para reenviar NEW_COORD) */
    uint16_t  sync_gen;                /* réplica: generación de la sincronización en curso */
    double    last_newcoord_resend;
    int       snlist_dirty;
    float     est_down_kbps;
    uint16_t  coord_id;                /* réplica: quién es el coordinador */
    smnet_addr_t coord_addr;
    /* reportes de viewers de los últimos 10 s */
    struct { double t; uint16_t reporter, dsn, host; } rep[256];
    struct { uint16_t area, owner; uint8_t seen; } areas[COORD_MAX_AREAS];
    int          nareas;
    double       roster_last, roster_total_last;
    int       nrep;
    /* demotes diferidos */
    struct { uint16_t id; double at; uint32_t epoch; } pend[64];
    int       npend;
    /* moderación (smnet_node_coord_*): identidades bloqueadas, partida cerrada y quiénes estuvieron */
    struct { uint8_t pk[32]; double until; } blocked[128];
    int       nblocked;
    int       closed;
    uint8_t   members[2048][8];        /* prefijo de la identidad de los que entraron */
    int       nmembers, members_pos;
} coord_t;

/* ---------------------------------------------------------------- nodo */
typedef struct { uint16_t id; smnet_addr_t addr; uint8_t flags; } sninfo_t;
typedef struct { int len; uint8_t d[SMNET_MTU + 64]; } loopmsg_t;

struct smnet_node {
    smnet_node_cfg_t   cfg;
    smnet_send_fn      send;
    void*              user;
    smnet_on_entity_fn on_entity;
    smnet_on_msg_fn    on_msg;
    smnet_on_player_fn on_player;
    smnet_on_owner_fn  on_owner;
    uint8_t            pinfo[SMNET_MAX_PINFO], pinfo_len, pinfo_dirty;
    uint32_t           roster_epoch, told_area;
    uint8_t            known[SMNET_MAX_PLAYERS / 8];
    /* reparación de la lista (los ROSTER van sin reintento): el coordinador anuncia el total cada
       2 s; si no coincide un rato, se pide la lista completa y se quitan los que ya no están */
    uint16_t           roster_total, roster_req_total;
    double             roster_mis_since, roster_snap_req, roster_collect_at, roster_total_at;
    uint8_t            roster_collect, roster_self_gone, roster_req_fails;
    uint8_t            roster_seen[SMNET_MAX_PLAYERS / 8];
    uint16_t           owner_area, area_owner;
    app_bucket_t       app_bucket[3];
    uint16_t           app_mseq;
    uint16_t*          app_last;        /* [SMNET_MAX_PLAYERS] mayor nº de mensaje visto por origen */
    uint32_t*          app_bits;        /* ventana de 32 anteriores (duplicados por traspasos) */
    void*              app_out_down;    /* colas agrupadas por destino (app.c) */
    void*              app_out_relay;
    smnet_on_blob_fn   on_blob;
    uint16_t           id;
    uint32_t           epoch_seen;
    uint16_t           coord_id;
    smnet_addr_t       coord_addr;
    int                have_coord;
    smnet_addr_t       bootstrap;
    sninfo_t           snlist[SMNET_MAX_SN];
    int                nsn;
    peer_t             peer;
    sn_t*              sn;
    coord_t*           co;
    smnet_stats_t      stats;
    link_t*            link;
    smnet_addr_t       public_addr;        /* como nos ve el coordinador (WELCOME) */
    uint8_t            game_pk[32], game_sk[64];   /* clave de la partida (la secreta: host y réplicas) */
    int                have_game, have_game_sk;
    uint8_t            cert[CERT_SIZE];    /* nuestro certificado (WELCOME) */
    int                have_cert;
    uint16_t           banned[BANLIST_MAX];   /* degradados según el coordinador vigente */
    int                nbanned;
    /* cola de entrega local (mensajes a uno mismo) */
    loopmsg_t*         loop;
    int                loop_n, loop_cap;
    double             now;
};

/* node.c */
void node_send_addr(smnet_node_t* n, const smnet_addr_t* to, const uint8_t* d, int len);
void node_send_id(smnet_node_t* n, uint16_t id, const smnet_addr_t* to, const uint8_t* d, int len);
void node_send_coord(smnet_node_t* n, const uint8_t* d, int len);
void node_hdr(smnet_node_t* n, wbuf_t* w, uint8_t type, uint8_t flags, uint16_t seq);
void node_become_sn(smnet_node_t* n, double now);
void node_stop_sn(smnet_node_t* n);
void node_set_snlist(smnet_node_t* n, const sninfo_t* list, int cnt);
void node_set_coord(smnet_node_t* n, uint16_t id, const smnet_addr_t* addr, uint32_t epoch);
void node_punch(smnet_node_t* n, const smnet_addr_t* to);
void node_ban_local(smnet_node_t* n, uint16_t id);
int  node_is_banned(const smnet_node_t* n, uint16_t id);
int  node_has_link(const smnet_node_t* n, const smnet_addr_t* to);
void node_send_intro(smnet_node_t* n, uint16_t to_id, const smnet_addr_t* to, uint16_t who, const smnet_addr_t* who_addr);

/* auth.c */
void auth_init_game(smnet_node_t* n);
void auth_make_cert(const smnet_node_t* n, uint16_t id, const uint8_t pk[32], uint8_t out[CERT_SIZE]);
int  auth_check_cert(const smnet_node_t* n, const uint8_t cert[CERT_SIZE]);
int  auth_build_present(const smnet_node_t* n, uint8_t* buf, int cap);
int  auth_on_present(smnet_node_t* n, const smnet_addr_t* from, rbuf_t* r);
int  auth_build_new_coord(const smnet_node_t* n, uint32_t epoch, uint8_t* buf, int cap);
int  auth_check_new_coord(const smnet_node_t* n, const smnet_addr_t* from, const smnet_hdr_t* h, const rbuf_t* r);
int  auth_allowed(smnet_node_t* n, const smnet_addr_t* from, const smnet_hdr_t* h);

/* roster.c */
void roster_peer_update(smnet_node_t* n);
int  roster_receive(smnet_node_t* n, const smnet_hdr_t* h, rbuf_t* r, int local);
int  roster_coord_receive(smnet_node_t* n, const smnet_hdr_t* h, rbuf_t* r);
void roster_coord_update(smnet_node_t* n, double now);

/* app.c */
int  app_on_up(smnet_node_t* n, const smnet_hdr_t* h, rbuf_t* r, int local, double now);
int  app_on_down(smnet_node_t* n, const smnet_hdr_t* h, rbuf_t* r, int local, double now);
void app_flush(smnet_node_t* n);
void app_free(smnet_node_t* n);

/* proof.c */
int  proof_init(smnet_node_t* n);
void proof_free(smnet_node_t* n);
void proof_note_sent(smnet_node_t* n, uint8_t seq, const uint8_t st[SMNET_STATE_SIZE]);
void proof_note_received(smnet_node_t* n, uint16_t id, uint8_t seq, const uint8_t st[SMNET_STATE_SIZE]);
int  proof_on_forward(smnet_node_t* n, rbuf_t* r, double now);
void proof_update(smnet_node_t* n, double now);
int  sn_on_proof(smnet_node_t* n, const smnet_hdr_t* h, rbuf_t* r, int relayed);
int  sn_on_verify_set(smnet_node_t* n, const smnet_hdr_t* h, rbuf_t* r);
void sn_forward_proofs(smnet_node_t* n);
int  sn_evil_drop(const smnet_node_t* n, uint16_t viewer, uint16_t e, uint8_t seq);

/* direct.c */
int  direct_init(smnet_node_t* n);
void direct_free(smnet_node_t* n);
void direct_update(smnet_node_t* n, double now);
void direct_send_state(smnet_node_t* n, const uint8_t* msg, int len, const uint8_t* blobmsg, int bloblen);
int  direct_receive(smnet_node_t* n, const smnet_addr_t* from, const smnet_hdr_t* h, rbuf_t* r, double now);
void direct_note(smnet_node_t* n, uint16_t id, uint8_t seq, double now, int via_sn);
int  sn_forward_link(smnet_node_t* n, const smnet_addr_t* from, const smnet_hdr_t* h, rbuf_t* r);
int  peer_accept_remote(smnet_node_t* n, uint16_t id, uint8_t seq, const smnet_state_t* st, double now);

/* peer.c */
void peer_init(smnet_node_t* n);
void peer_free(smnet_node_t* n);
void peer_update(smnet_node_t* n, double now);
void peer_force_rejoin(smnet_node_t* n, double now);
void peer_probe_coord(smnet_node_t* n, double now);
int  peer_receive(smnet_node_t* n, const smnet_addr_t* from, const smnet_hdr_t* h, rbuf_t* r, double now);

/* sn.c */
sn_t* sn_new(smnet_node_t* n, double now);
void  sn_free(sn_t* s);
void  sn_update(smnet_node_t* n, double now);
int   sn_receive(smnet_node_t* n, const smnet_addr_t* from, const smnet_hdr_t* h, rbuf_t* r, double now);
void  sn_sync_others(smnet_node_t* n);
int   sn_hosted_count(const sn_t* s, double now, double expire);

/* coord.c */
coord_t* coord_new(void);
void  coord_free(coord_t* c);
void  coord_start_active(smnet_node_t* n, double now);
void  coord_update(smnet_node_t* n, double now);
int   coord_receive(smnet_node_t* n, const smnet_addr_t* from, const smnet_hdr_t* h, rbuf_t* r, double now);

#endif
