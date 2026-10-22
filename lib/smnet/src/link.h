/* Capa de enlace (PROTOCOL.md §4, v1.3): sesiones cifradas por dirección, canal confiable
 * ordenado para el control y perforación de NAT. Sans-IO, como el resto del núcleo.
 *
 *   handshake 1 RTT : INIT (64 B) / RESP (57 B), X25519 efímero + BLAKE2b
 *   datos           : DATA = 0xA3 | sid destino u32 | pn u64 | AEAD(XChaCha20-Poly1305) | mac 16
 *                     (29 B de overhead). La ventana anti-replay es también el estado de acks.
 *   confiable       : orden por sesión, acks a caballito o puros a los 20 ms, RTO adaptativo.
 *   perforación     : PUNCH (5 B, en claro) abre el filtro del NAT propio hacia una dirección.
 *
 * Sin identidad todavía: el hito 4 agrega las claves Ed25519 al handshake. */
#ifndef SMNET_LINK_H
#define SMNET_LINK_H

#include <stdint.h>
#include "smnet.h"

#define LINK_MAX_SESS     2048
#define LINK_OVERHEAD     29          /* DATA: tipo + sid + pn + mac */
#define LINK_MAX_INNER    (SMNET_MTU + 64)

typedef struct link link_t;
typedef void (*link_out_fn)(void* ctx, const smnet_addr_t* to, const uint8_t* d, int len);
typedef void (*link_in_fn)(void* ctx, const smnet_addr_t* from, const uint8_t* msg, int len);

typedef smnet_link_stats_t link_stats_t;

typedef struct {
    double hs_retry_ms;       /* 250 */
    int    hs_tries;          /* 16 (~4 s: da tiempo a que llegue la perforación del otro lado) */
    double idle_expire_ms;    /* 30000 sin recibir nada */
    double ack_delay_ms;      /* 20 */
    int    encrypt;           /* 1; 0 solo para bancos de escala (mismo formato, sin AEAD ni firmas) */
    double wall_offset_ms;    /* reloj de pared = now + esto (marca de tiempo creciente del INIT) */
    uint8_t identity_seed[32];/* en cero: identidad al azar (de la semilla) */
} link_cfg_t;

void link_cfg_default(link_cfg_t* c);
link_t* link_new(const link_cfg_t* cfg, const uint8_t seed[32], link_out_fn out, link_in_fn in, void* ctx);
void link_free(link_t* l);

/* Manda un mensaje (ya con cabecera smnet). Si no hay sesión, la abre y lo encola. */
void link_send(link_t* l, const smnet_addr_t* to, const uint8_t* msg, int len, int reliable, double now);
void link_receive(link_t* l, const smnet_addr_t* from, const uint8_t* d, int len, double now);
void link_update(link_t* l, double now);
/* Datagrama chico en claro para abrir el filtro del NAT propio hacia `to`. */
void link_punch(link_t* l, const smnet_addr_t* to, double now);
int  link_established(const link_t* l, const smnet_addr_t* to);
float link_srtt(const link_t* l, const smnet_addr_t* to);    /* 0 si no hay muestra */
void link_close(link_t* l, const smnet_addr_t* to);
int  link_pending(const link_t* l, const smnet_addr_t* to);   /* confiables sin confirmar (-1: sin sesión) */
const link_stats_t* link_stats(const link_t* l);

/* Identidad (Ed25519, derivada de la semilla). La del otro lado se verifica en el handshake. */
const uint8_t* link_identity(const link_t* l);
void link_sign(const link_t* l, uint8_t sig[64], const uint8_t* msg, int len);
const uint8_t* link_peer_identity(const link_t* l, const smnet_addr_t* a);   /* NULL sin sesión */
/* Etiqueta de la aplicación por sesión: el id de jugador ya verificado (SMNET_NO_ID si no). Se
 * borra sola si el otro lado cambia de identidad. */
uint16_t link_tag(const link_t* l, const smnet_addr_t* a);
void link_set_tag(link_t* l, const smnet_addr_t* a, uint16_t tag);
int  link_presented(const link_t* l, const smnet_addr_t* a);
void link_set_presented(link_t* l, const smnet_addr_t* a);
/* Mensaje de presentación de la aplicación: el enlace lo pone primero en cada sesión nueva y cada
 * vez que el otro lado perdió el estado de la sesión (y lo manda ya por las abiertas si cambió). */
void link_set_presentation(link_t* l, const uint8_t* msg, int len);

/* CSPRNG determinista a partir de la semilla (BLAKE2b con contador). */
void link_random(link_t* l, uint8_t* out, int n);

#endif
