#ifndef NS_SMNET_LOBBY_H
#define NS_SMNET_LOBBY_H
/* Partidas masivas listadas en coopnet (nuestro servidor coopnet modificado, ver coopnet/MASIVO.md):
   el host mantiene un lobby de coopnet y se registra en el encuentro UDP desde su socket de smnet;
   el que se une le pregunta al encuentro la dirección del host. */
#ifdef SMNET
#include <stdbool.h>
#include <stdint.h>
#include "smnet.h"

#define RV_PROBE     1
#define RV_PROBE_R   2
#define RV_REG       3
#define RV_REG_R     4
#define RV_JOIN      5
#define RV_JOIN_R    6
#define RV_PUNCH     7
#define RV_REPORT    8
#define RV_TELEM     9    /* telemetría anónima (ver ns_smnet_telemetry_send) */
#define RV_VERSION   10   /* ¿cuál es la última versión del fork? */
#define RV_VERSION_R 11

/* versión del fork: SMNET_FORK_BUILD / SMNET_FORK_LABEL en pc/network/version.h */

enum { NS_NAT_OPEN = 0, NS_NAT_CONE = 1, NS_NAT_SYMMETRIC = 2, NS_NAT_UNKNOWN = 3 };

/* el encuentro: el servidor coopnet configurado (coopnet_ip) y sus puertos UDP (coopnet_port+1 y +2;
   COOPDX_COOPNET=ip:puerto para probar con otro) */
bool ns_rv_server_addr(int which, smnet_addr_t* out);
bool ns_rv_is_msg(const uint8_t* d, int len);
int  ns_rv_build_probe(uint8_t* b, uint32_t nonce);
int  ns_rv_build_join(uint8_t* b, uint64_t lobbyId, uint32_t nonce, const char* password);
int  ns_rv_build_report(uint8_t* b, int natType);
/* tipo de NAT a partir de las dos respuestas de la sonda (puerto local del socket, en orden de host) */
int  ns_rv_nat_from_probes(uint16_t localPort, uint16_t pubA, uint16_t pubB, bool gotA, bool gotB);
const char* ns_rv_nat_name(int nat);
const char* ns_rv_join_error(int status);

/* el que se une: tipo de NAT medido (lo usa el nodo de smnet al crearse) y la partida, para que el
   host perfore también hacia el socket del juego */
void ns_smnet_set_nat_hint(int nat);
int  ns_smnet_nat_hint(void);
void ns_smnet_set_join_lobby(uint64_t lobbyId, const char* password);

/* host de una partida masiva por coopnet (más de 16 en el panel de Host con CoopNet) */
void ns_smnet_lobby_host_request(void);
bool ns_smnet_lobby_host_requested(void);
void ns_smnet_lobby_host_start(void);     /* desde ns_smnet: ya hay socket y nodo */
void ns_smnet_lobby_stop(void);
void ns_smnet_lobby_update(int fd, int players, double now);
/* mensajes del encuentro que llegaron al socket del juego */
void ns_smnet_lobby_rv_rx(int fd, const smnet_addr_t* from, const uint8_t* d, int len, double now);
/* el que se une (desde ns_smnet): pedirle al encuentro que el host perfore hacia este socket */
void ns_smnet_lobby_join_ping(int fd, double now);

/* aviso de versión nueva del fork (en vez del actualizador oficial, que instalaría la versión
   oficial encima): pregunta al servidor coopnet configurado; true y el texto del aviso si hay una
   más nueva. Tarda como mucho ~1,5 s si el servidor no contesta. */
bool ns_smnet_fork_update_check(char* out, int cap);

/* telemetría anónima de las partidas masivas: cada 30 s al encuentro (sin nombres ni direcciones;
   un número de sesión al azar por partida). COOPDX_SMNET_TELEMETRIA=0 la apaga. */
typedef struct {
    uint8_t  flags;            /* bit 0 host, 1 coordinador, 2 SN, 3 conectado */
    uint16_t players;          /* los de la partida que conoce (con uno mismo) */
    uint8_t  slots;            /* jugadores de coopdx cercanos (máx. 15) */
    uint16_t impostors;        /* dibujados como impostores ahora */
    uint16_t disconnects;      /* desde el informe anterior: veces que perdió la conexión */
    uint16_t failovers;        /* cambios de SN */
    uint16_t rejoins;          /* reentradas de smnet */
    uint16_t hs_failed;        /* apretones de manos fallidos */
    uint8_t  retx_pct;         /* retransmisiones del canal confiable, % */
    uint16_t rx_kbps, tx_kbps;
    uint16_t sn_hosted;        /* si es SN: peers alojados */
    uint32_t uptime_s;         /* desde que entró a la partida */
} ns_telem_t;
void ns_smnet_note_fps(unsigned fps);     /* desde pc_main, una vez por segundo */
bool ns_smnet_telemetry_enabled(void);
void ns_smnet_telemetry_new_session(void);
void ns_smnet_telemetry_send(int fd, const ns_telem_t* t);

/* Join desde la lista de partidas (djui_panel_join_lobbies.c) */
struct DjuiBase;
bool ns_smnet_prejoin_begin_lobby(struct DjuiBase* caller, uint64_t lobbyId, const char* password);
#endif
#endif
