/*
 * smnet — red de supernodos para sm64coopdx (PROTOCOL.md v1.2), hito 2.
 *
 * Núcleo "sans-IO": no abre sockets ni lee el reloj. La aplicación le entrega los datagramas
 * recibidos y la hora; el núcleo llama a `send` con lo que hay que mandar.
 *
 * Hito 2: todo participante es un NODO con roles dinámicos:
 *   - peer (siempre): entra por el coordinador, manda su estado a su SN primario, mantiene un
 *     backup con heartbeats, mide RTT a los SN y hace failover si el primario se cae;
 *   - supernodo (si el coordinador lo promueve): atiende a sus peers, reenvía estado a los otros
 *     SN (base 2 Hz + suscripciones a 10/30 Hz) y arma los lotes por cercanía;
 *   - coordinador (el host, o una réplica que toma el control): elección de SN, asignación por
 *     costo mínimo, backups, réplicas en caliente con fencing por época.
 */
#ifndef SMNET_H
#define SMNET_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMNET_PROTO_VERSION   1
#define SMNET_MAX_PLAYERS     1024
#define SMNET_MAX_SN          64
#define SMNET_MTU             1200
#define SMNET_MAX_BLOB        480
#define SMNET_MAX_FULL        32     /* tope de sn.full_k (vecinos con estado rico por viewer) */
#define SMNET_NO_ID           0xFFFF

enum { SMNET_NAT_OPEN = 0, SMNET_NAT_CONE = 1, SMNET_NAT_SYMMETRIC = 2 };

typedef struct {
    uint8_t len;
    uint8_t data[28];
} smnet_addr_t;

int smnet_addr_eq(const smnet_addr_t* a, const smnet_addr_t* b);

typedef struct {
    float   x, y, z;
    int16_t yaw;
    uint8_t flags;
    float   fwd_vel, vel_y;
    uint8_t action, anim_frame;
} smnet_state_t;

typedef void (*smnet_send_fn)(void* user, const smnet_addr_t* to, const uint8_t* data, int len);
typedef void (*smnet_on_entity_fn)(void* user, uint16_t id, uint8_t seq, const smnet_state_t* st, double now_ms);
typedef void (*smnet_on_blob_fn)(void* user, uint16_t id, uint8_t seq, const uint8_t* blob, int len, double now_ms);

/* ---- API v1: mensajes de la aplicación (PROTOCOL.md §6.4) ---- */
typedef enum { SMNET_TO_AREA = 1, SMNET_TO_ALL = 2, SMNET_TO_PLAYER = 3, SMNET_TO_COORD = 4 } smnet_scope_t;
#define SMNET_MAX_APP 1100
typedef void (*smnet_on_msg_fn)(void* user, uint16_t from, smnet_scope_t scope, const uint8_t* data, int len, double now_ms);
#define SMNET_MAX_PINFO 64
/* Alta (joined = 1, también para cambios de info) o baja (joined = 0) de un jugador, de todos los
 * de la partida y no solo los visibles. Quien entra recibe primero la lista completa. */
typedef void (*smnet_on_player_fn)(void* user, uint16_t id, int joined, const uint8_t* info, int info_len);
/* Cambió el dueño de nuestra área (lo elige el coordinador). */
typedef void (*smnet_on_owner_fn)(void* user, uint16_t area, uint16_t owner);

/* ------------------------------------------------------------- configuración */

typedef struct {
    double   down_budget_kbps;      /* 600 */
    double   full_budget_kbps;      /* 160 */
    int      tier_k[2];             /* {32, 128} */
    float    tier_hz[3];            /* {30, 10, 2} */
    int      full_k;                /* 4 */
    float    full_radius;           /* 1500 */
    int      rank_every_ticks;      /* 10 */
    int      max_datagrams_per_tick;/* 3 */
    float    relay_base_hz;         /* 2: todo lo alojado va a todos los SN al menos a esta tasa */
    double   subscribe_every_ms;    /* 250 */
    double   subscribe_ttl_ms;      /* 3000 */
    double   hosted_expire_ms;      /* 1000: sin STATE_UP, el peer deja de estar alojado */
    double   remote_stale_ms;       /* 2500: entidad remota sin updates deja de rankearse */
    double   report_every_ms;       /* 1000: SN_REPORT al coordinador */
    /* SOLO PARA PRUEBAS: SN malicioso (ver smnet_node_set_evil) */
    float    evil_drop;             /* fracción de entidades que no manda a sus viewers */
    int      evil_alter;            /* altera las posiciones que manda */
    int      evil_drop_proofs;      /* no reenvía las pruebas firmadas */
} smnet_sn_cfg_t;

typedef struct {
    double   rebalance_ms;          /* 10000 */
    double   ondemand_gap_ms;       /* 1000 */
    uint32_t sn_min_upload_kbps;    /* 20000 */
    double   sn_min_uptime_ms;      /* 30000 (se ignora si falta capacidad) */
    float    sn_usable;             /* 0.6 */
    float    margin, demote_margin; /* 1.25, 1.7 */
    int      max_fanout;            /* 48 */
    float    hysteresis_ms;         /* 15 */
    float    beta_ms;               /* 10 */
    float    max_move_frac;         /* 0.05 */
    float    unknown_rtt_ms;        /* 250 */
    float    emergency_overflow;    /* 1.25 */
    double   sn_dead_ms;            /* 6000 sin SN_REPORT */
    double   peer_dead_ms;          /* 15000 sin noticias del peer */
    int      replicas;              /* 2 */
    double   hb_every_ms;           /* 250: COORD_HB a las réplicas */
    double   takeover_ms;           /* 750 (× (rango+1)) */
    double   sync_every_ms;         /* 2000: COORD_SYNC completo */
    double   first_rebalance_ms;    /* 2000 tras tomar el control */
} smnet_coord_cfg_t;

typedef struct {
    uint32_t nonce;                 /* identifica al nodo ante el coordinador (estable entre reintentos) */
    uint32_t upload_kbps;           /* upload medido */
    uint8_t  nat;                   /* SMNET_NAT_* */
    int      is_host;               /* arranca como coordinador */
    smnet_addr_t self_addr;         /* dirección propia como la ven los demás (obligatoria en el host) */
    uint8_t  seed[32];              /* OBLIGATORIA con cifrado: 32 bytes de entropía del sistema
                                       (getrandom / BCryptGenRandom / arc4random_buf). En cero,
                                       smnet_node_new devuelve NULL (salvo encrypt = 0). */
    uint8_t  identity_seed[32];     /* opcional: semilla de la identidad (la clave del enlace). Guardada
                                       en disco hace que la identidad dure entre partidas (los baneos
                                       se aplican a ella); en cero, una nueva al azar cada vez. */
    int      encrypt;               /* 1; 0 solo en bancos de escala (mismo formato sin AEAD ni firmas) */
    int      verify_neighbors;      /* 1: verifica las firmas de los estados de sus vecinos (detecta SN que
                                       alteran o tiran estados). 0 solo para bots de prueba: ahorra ~70 % de CPU */
    double   wall_offset_ms;        /* reloj de pared - now (marcas de tiempo del handshake) */
    uint16_t requested_id;          /* id que se pide (p. ej. el número de jugador); SMNET_NO_ID = cualquiera */
    uint8_t  game_pk[32];           /* opcional: clave de la partida fijada por el lobby (en cero = se
                                       confía en la del nodo por el que se entra) */
    int      direct;                /* 1: atajo directo estable (PROTOCOL §9.4) */
    int      direct_max;            /* 8 enlaces (máx. 16) */
    float    direct_upload_frac;    /* 0.3 del upload */
    double   direct_eval_ms;        /* 5000 */
    double   hello_retry_ms;        /* 500 */
    double   primary_timeout_ms;    /* 750 */
    float    app_rate_direct;       /* mensajes a un jugador o al coordinador por segundo (30); la ráfaga es el doble */
    double   backup_dead_ms;        /* 3000 */
    double   backup_hb_ms;          /* 1000 */
    double   metrics_every_ms;      /* 10000 */
    double   first_metrics_ms;      /* 1500 */
    double   ping_cycle_ms;         /* 5000: se mide RTT a cada SN una vez por ciclo */
    double   remote_expire_ms;      /* 3000 */
    smnet_sn_cfg_t    sn;
    smnet_coord_cfg_t coord;
} smnet_node_cfg_t;

void smnet_node_cfg_default(smnet_node_cfg_t* c);

/* -------------------------------------------------------------------- nodo */

typedef struct smnet_node smnet_node_t;

/* Capa de enlace (sesiones cifradas, canal confiable, perforación de NAT). */
typedef struct {
    uint64_t hs_started, hs_ok, hs_failed, hs_responded, hs_simultaneous, hs_restarts;
    uint64_t rel_sent, rel_retx, rel_dropped, rel_delivered;
    uint64_t data_tx, data_rx, bad_mac, replays, no_session, malformed;
    uint64_t punches_tx, punches_rx, roams, expired;
    uint64_t pure_acks, bad_sig;
    int      sessions;
} smnet_link_stats_t;

typedef enum { SMNET_PEER_IDLE, SMNET_PEER_JOINING, SMNET_PEER_CONNECTED } smnet_peer_status_t;
typedef enum { SMNET_COORD_NONE, SMNET_COORD_ACTIVE, SMNET_COORD_REPLICA } smnet_coord_role_t;

typedef struct {
    smnet_peer_status_t status;
    uint16_t id, primary, backup, coord_id;
    uint32_t epoch;
    int      is_sn;
    smnet_coord_role_t coord_role;
    int      hosted;                /* si es SN: peers alojados */
    int      n_sn;                  /* SN que conoce */
    uint32_t failovers, rejoins;
    double   last_data_ms;          /* último lote recibido */
    int      n_direct;              /* atajos directos activos */
    uint16_t direct_ids[16];
    uint32_t direct_opens, direct_attempts, direct_failed, direct_nogain, direct_lost;
    uint32_t direct_f_noanswer, direct_f_rejected, direct_f_nat, direct_f_timeout;
    double   direct_life_mean_s;    /* vida media de los enlaces cerrados */
    double   last_failover_ms;
    int      last_failover_kind;    /* 1 al backup, 2 salto al SN de menor RTT */
} smnet_node_info_t;

typedef struct {
    uint64_t tx_bytes, rx_bytes, tx_packets, rx_packets, bad_packets;
    uint64_t entity_updates, blob_updates;
    /* como SN */
    uint64_t sn_entities_sent, sn_entities_cut, sn_relay_entities, sn_blobs_sent;
    /* como coordinador */
    uint64_t co_rebalances, co_promotions, co_demotions, co_moves, co_sn_deaths, co_takeovers;
    /* seguridad */
    uint64_t unauthorized;          /* mensajes descartados: remitente sin verificar o sin permiso */
    uint64_t auth_rejected;         /* certificados / claves rechazados */
    uint64_t hello_hijack;          /* HELLO con un nonce ajeno desde otra identidad */
    uint64_t proofs_ok, proofs_bad, altered_detected, gap_reports;   /* como viewer */
    uint64_t ver_emitted, ver_got;  /* estados firmados de los vecinos verificados / recibidos de esos */
    uint64_t co_reports, co_bans, co_bans_temp;                      /* como coordinador */
    uint64_t app_tx, app_rx, app_rate_limited, app_dup;                       /* mensajes de la aplicación */
    double   co_last_ban_ms;
    uint16_t co_last_banned;
    double   co_last_solve_ms;      /* lo completa quien llama, si quiere medirlo */
} smnet_stats_t;

smnet_node_t* smnet_node_new(const smnet_node_cfg_t* cfg, smnet_send_fn send, void* user);
void smnet_node_free(smnet_node_t* n);
void smnet_node_set_callbacks(smnet_node_t* n, smnet_on_entity_fn on_entity, smnet_on_blob_fn on_blob);
/* coordinator: dirección del coordinador (o de cualquier nodo de la partida); NULL en el host. */
void smnet_node_start(smnet_node_t* n, const smnet_addr_t* coordinator, double now_ms);
void smnet_node_set_local(smnet_node_t* n, uint16_t area, const smnet_state_t* st, const uint8_t* blob, int blob_len);
void smnet_node_receive(smnet_node_t* n, const smnet_addr_t* from, const uint8_t* data, int len, double now_ms);
void smnet_node_update(smnet_node_t* n, double now_ms);
void smnet_node_info(const smnet_node_t* n, smnet_node_info_t* out);
uint8_t smnet_node_seq(const smnet_node_t* n);          /* seq del último STATE_UP */
const smnet_stats_t* smnet_node_stats(const smnet_node_t* n);
const smnet_link_stats_t* smnet_node_link_stats(const smnet_node_t* n);
/* SOLO PARA PRUEBAS: vuelve malicioso al nodo cuando actúa de SN. */
void smnet_node_set_evil(smnet_node_t* n, float drop, int alter, int drop_proofs);

/* Avisa a los demás que se va (LEAVE al primario). */
void smnet_node_leave(smnet_node_t* n, double now_ms);

/* Mensaje de la aplicación (≤ SMNET_MAX_APP B), confiable en cada salto. Área: a los de nuestra
 * área (la de smnet_node_set_local); todos: a todos; jugador: a `target`; coordinador: al activo.
 * Límite de tasa por jugador: todos 4/s (ráfaga 16), área 60/s (ráfaga 120), jugador/coordinador
 * 30/s (ráfaga 60). Devuelve 0; -1 sin conectar; -2 inválido o muy grande; -3 por la tasa. */
int  smnet_node_send_msg(smnet_node_t* n, smnet_scope_t scope, uint16_t target, const uint8_t* data, int len);
void smnet_node_set_app_callbacks(smnet_node_t* n, smnet_on_msg_fn on_msg, smnet_on_player_fn on_player, smnet_on_owner_fn on_owner);
/* Datos de presentación propios (nombre, color, modelo; ≤ SMNET_MAX_PINFO B): se reparten a todos. */
void smnet_node_set_player_info(smnet_node_t* n, const uint8_t* info, int len);
/* Dueño vigente de nuestra área, o SMNET_NO_ID si todavía no se sabe. */
uint16_t smnet_node_area_owner(const smnet_node_t* n);

/* Moderación (la decide la aplicación; esto solo actúa en el coordinador activo y devuelve -1 en
 * los demás nodos). La lista no se replica: la aplicación la vuelve a aplicar si otro nodo pasa a
 * ser coordinador.
 *   identity: la identidad (clave pública del enlace) del nodo id.
 *   expel: lo saca de la tabla y bloquea su identidad block_ms (< 0: para siempre; 0: no la bloquea);
 *          su id no se reasigna mientras dure el bloqueo (al menos 10 min).
 *   block: bloquea una identidad (HELLO ignorado) hasta now + block_ms (< 0: para siempre).
 *   unblock_all: saca todos los bloqueos.
 *   set_closed: cerrada, solo vuelven a entrar identidades que ya estuvieron en la partida. */
/* la identidad propia (para que otro la reconozca, por ejemplo como administrador) */
void smnet_node_identity(const smnet_node_t* n, uint8_t out[32]);
int  smnet_node_coord_identity(const smnet_node_t* n, uint16_t id, uint8_t out[32]);
int  smnet_node_coord_expel(smnet_node_t* n, uint16_t id, double block_ms, double now);
int  smnet_node_coord_block(smnet_node_t* n, const uint8_t pk[32], double block_ms, double now);
int  smnet_node_coord_unblock_all(smnet_node_t* n);
int  smnet_node_coord_set_closed(smnet_node_t* n, int closed);

#ifdef __cplusplus
}
#endif
#endif
