/* Formato de cable de PROTOCOL.md v1.1 (little-endian). */
#ifndef SMNET_WIRE_H
#define SMNET_WIRE_H

#include "smnet.h"

#define SMNET_HDR_SIZE      8
#define SMNET_STATE_SIZE    12
#define SMNET_ENTITY_SIZE   15   /* id u16 + seq u8 + estado 12 */
#define SMNET_RELAY_SIZE    17   /* id u16 + seq u8 + área u16 + estado 12 */

enum {
    MSG_HELLO       = 0x01,
    MSG_WELCOME     = 0x02,
    MSG_ASSIGN      = 0x03,
    MSG_PROMOTE     = 0x04,
    MSG_DEMOTE      = 0x05,
    MSG_METRICS     = 0x06,
    MSG_SN_REPORT   = 0x07,
    MSG_HEARTBEAT   = 0x08,   /* flag HB_FROM_SN: lo manda un SN (respuesta o latido) */
    MSG_FAILOVER    = 0x09,
    MSG_STATE_UP    = 0x10,
    MSG_STATE_BATCH = 0x11,
    MSG_STATE_RELAY = 0x12,
    MSG_STATE_FULL  = 0x15,
    MSG_COORD_SYNC  = 0x20,
    MSG_LEAVE       = 0x25,
    MSG_SN_LIST     = 0x28,
    MSG_PING        = 0x29,
    MSG_PONG        = 0x2A,
    MSG_SUBSCRIBE   = 0x2B,
    MSG_RELAY_FULL  = 0x2C,
    MSG_NEW_COORD   = 0x2D,
    MSG_HELLO_FWD   = 0x2E,
    MSG_COORD_HB    = 0x2F,
    MSG_INTRO       = 0x30,   /* "perforá hacia esta dirección": coordinador o SN -> SN */
    MSG_INTRO_REQ   = 0x31,   /* peer -> su primario: presentame a estos SN */
    MSG_PRESENT     = 0x32,   /* clave de la partida + certificado: al abrir cada sesión */
    MSG_REDIRECT    = 0x33,
    MSG_APP_UP      = 0x40,   /* v1: mensaje de la aplicación hacia el SN o el coordinador */
    MSG_APP_DOWN    = 0x41,   /* v1: mensaje de la aplicación hacia el destinatario */
    MSG_PLAYER_INFO = 0x42,   /* v1: datos de presentación del jugador, al coordinador */
    MSG_ROSTER      = 0x43,   /* v1: altas y bajas de jugadores */
    MSG_AREA_OWNER  = 0x44,   /* v1: dueño de un área */
    MSG_AREA_NOTICE = 0x45,   /* v1: "cambié de área", al coordinador */
    MSG_STATE_PROOF = 0x16,   /* emisor -> su SN: hashes de los estados del último segundo, firmados */
    MSG_PROOF_FWD   = 0x17,   /* SN -> viewers cercanos */
    MSG_RELAY_PROOF = 0x18,   /* SN -> SN suscriptos a 30 Hz */
    MSG_GAP_REPORT  = 0x0E,
    MSG_VERIFY_SET  = 0x19,   /* viewer -> su SN: los vecinos que verifico (mandame sus pruebas) */   /* viewer -> coordinador: alteración, descarte o pruebas que no llegan */   /* "el coordinador es este": respuesta a un HELLO que no era para mí */
    MSG_LINK_OFFER  = 0x0B,   /* atajo directo: oferta (peer -> SN -> SN -> peer) */
    MSG_LINK_ANSWER = 0x0C,
    MSG_LINK_CLOSE  = 0x0D,
    MSG_STATE_DIRECT= 0x13,   /* peer <-> peer por el atajo */
    MSG_DIRECT_SET  = 0x14,   /* peer -> su SN: estos vecinos me llegan directo (bajalos a 2 Hz) */
};

#define HB_FROM_SN      0x02     /* flag de cabecera en HEARTBEAT */
#define UP_HAS_BLOB     0x01     /* flag de cabecera en STATE_UP */
#define SNF_OPEN        0x01     /* SN_LIST: el SN tiene NAT abierto */
#define SNF_REPLICA     0x02     /* SN_LIST: es réplica del coordinador */

typedef struct {
    uint8_t  version;
    uint8_t  flags;
    uint8_t  type;
    uint16_t sender;
    uint16_t seq;
    uint16_t tick;
} smnet_hdr_t;

/* Escritor/lector con control de límites. */
typedef struct { uint8_t* p; int len, cap, err; } wbuf_t;
typedef struct { const uint8_t* p; int len, pos, err; } rbuf_t;

void w_u8(wbuf_t* w, uint8_t v);
void w_u16(wbuf_t* w, uint16_t v);
void w_u32(wbuf_t* w, uint32_t v);
void w_bytes(wbuf_t* w, const void* src, int n);
uint8_t  r_u8(rbuf_t* r);
uint16_t r_u16(rbuf_t* r);
uint32_t r_u32(rbuf_t* r);
void     r_bytes(rbuf_t* r, void* dst, int n);

void hdr_write(wbuf_t* w, const smnet_hdr_t* h);
int  hdr_read(rbuf_t* r, smnet_hdr_t* h);   /* 0 si es válido */

void addr_write(wbuf_t* w, const smnet_addr_t* a);
void addr_read(rbuf_t* r, smnet_addr_t* a);

void state_write(wbuf_t* w, const smnet_state_t* s);
void state_read(rbuf_t* r, smnet_state_t* s);
/* Lo que sobrevive a la cuantización (para tests y para comparar). */
void state_quantize(const smnet_state_t* in, smnet_state_t* out);

#endif
