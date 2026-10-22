/* modsync: lista de mods de la partida y descarga de los que faltan, entre jugadores, sobre los
   mensajes de aplicación de smnet. No depende del juego (lo usan ns_smnet.c y la herramienta
   smnet-mod-clientes).

   - Manifiesto: los mods activos de la partida (carpeta o archivo suelto) con cada archivo, su
     tamaño y su huella (BLAKE2b de 16 B). Su id es la huella de todo el manifiesto.
   - Semilla: cualquiera que tenga los mods de la partida (el host y todos los que ya entraron)
     contesta el manifiesto y los pedazos que le piden. Atiende a pocos a la vez (lo demás va a una
     lista de espera, con su posición) y con un tope de pedazos por segundo.
   - Descarga: pide el manifiesto, ve qué archivos le faltan o tiene distintos, pide los pedazos a
     varias semillas a la vez (los mensajes viajan por las ramas de supernodos de smnet), verifica
     la huella de cada archivo al completarlo y lo entrega para guardar. Si una semilla no
     contesta, la deja; si está ocupada, espera o usa otra. */
#ifndef MODSYNC_H
#define MODSYNC_H
#include <stdint.h>
#include <stdbool.h>

#define MS_MSG_FIRST   0x90
#define MS_MANIFEST_REQ 0x90   /* [] */
#define MS_MANIFEST     0x91   /* [parte u8][total u8][bytes] */
#define MS_CHUNK_REQ    0x92   /* [id 8 B]{[archivo u16][desde u32][largo u16]}... */
#define MS_CHUNK        0x93   /* [id 8 B][archivo u16][desde u32][datos] */
#define MS_BUSY         0x94   /* [id 8 B][posición u16] */
#define MS_NOPE         0x95   /* [id 8 B]: no tengo esa versión */
#define MS_HAVE_REQ     0x96   /* [id 8 B]: ¿qué bloques tenés? */
#define MS_HAVE         0x97   /* [id 8 B][desde u32][cuántos u16][bits]; cuántos = 0xFFFF: todos */
#define MS_MSG_LAST    0x97
#define MS_CHUNK_DATA   1024
#define MS_BLOCK        65536   /* cada bloque tiene su huella en el manifiesto: se verifica al llegar y
                                   se puede repartir enseguida, aunque el archivo no esté completo */
#define MS_NOT_READY    0xFFFF  /* posición en MS_BUSY: "todavía estoy bajando, no soy semilla" */

typedef struct {
    char     path[200];      /* "carpeta-del-mod/sub/archivo.lua" o "mod-suelto.lua" */
    uint32_t size;
    uint8_t  hash[16];
    uint32_t block0, nblocks;   /* bloques de MS_BLOCK: índice global del primero y cuántos */
} ms_file_t;

typedef struct {
    int        nmods, nfiles;
    char     (*mods)[96];    /* nombres (carpeta o archivo) en el orden de activación */
    ms_file_t* files;
    uint8_t    id[16];
    uint32_t   nblocks;
    uint8_t  (*bhash)[16];   /* huella de cada bloque (índice global) */
    uint8_t*   blob;         /* serializado, lo que viaja */
    int        blob_len;
    uint64_t   total_bytes;
    int        has_sig;      /* firmado por el host de la partida (verificado al leerlo) */
    uint8_t    pub[32], sig[64];
} ms_manifest_t;

typedef struct {
    void* user;
    /* manda un mensaje de aplicación a un jugador (0 = salió) */
    int  (*send)(void* user, uint16_t to, const uint8_t* d, int len);
    /* semilla: lee un pedazo del archivo i del manifiesto (bytes leídos, <0 error) */
    int  (*read)(void* user, int file, uint32_t off, uint8_t* buf, int len);
    /* descarga: 1 si ya está el archivo con ese tamaño y huella */
    int  (*have)(void* user, const ms_file_t* f);
    /* descarga: archivo completo y verificado (0 = guardado) */
    int  (*write)(void* user, const ms_file_t* f, const uint8_t* data, uint32_t len);
} ms_io_t;

/* manifiesto ----------------------------------------------------------------------------------- */
void ms_manifest_init(ms_manifest_t* m);
void ms_manifest_free(ms_manifest_t* m);
/* agrega un mod: `root` es la carpeta donde está y `name` su carpeta o archivo (.lua) */
int  ms_manifest_add_mod(ms_manifest_t* m, const char* root, const char* name);
void ms_manifest_finish(ms_manifest_t* m);            /* calcula el id y serializa */
int  ms_manifest_parse(ms_manifest_t* m, const uint8_t* d, int len);
int  ms_hash_file(const char* path, uint32_t* size, uint8_t hash[16]);
void ms_hash_mem(const uint8_t* d, uint32_t len, uint8_t hash[16]);
void ms_hex(const uint8_t* h, int n, char* out);      /* out: 2n+1 */
/* firma (EdDSA de monocypher): la clave del host de la partida firma el id del manifiesto; la
   firma viaja al final del manifiesto (no cuenta para el id) y se verifica al leerlo */
void ms_key_pair(uint8_t sk[64], uint8_t pub[32], const uint8_t seed[32]);
void ms_sign(uint8_t sig[64], const uint8_t sk[64], const uint8_t* msg, int len);
int  ms_check(const uint8_t sig[64], const uint8_t pub[32], const uint8_t* msg, int len);   /* 0 = válida */
void ms_manifest_sign(ms_manifest_t* m, const uint8_t sk[64], const uint8_t pub[32]);
/* le pone a un manifiesto armado acá la firma del host que vino con el mismo id (-1 si no vale) */
int  ms_manifest_attach_sig(ms_manifest_t* m, const uint8_t pub[32], const uint8_t sig[64]);

/* semilla --------------------------------------------------------------------------------------- */
typedef struct ms_seeder ms_seeder_t;
typedef struct { unsigned long long pedidos_manifiesto, pedazos, bytes, en_espera_avisos; int atendiendo, en_espera; } ms_seeder_stats_t;
ms_seeder_t* ms_seeder_new(const ms_manifest_t* m, const ms_io_t* io, int max_active, int chunks_per_s);
/* semilla parcial: solo reparte los bloques para los que has_block da 1 (NULL = todos) */
void ms_seeder_set_partial(ms_seeder_t* s, int (*has_block)(void* user, uint32_t block), void* user);
void ms_seeder_free(ms_seeder_t* s);
bool ms_seeder_on_msg(ms_seeder_t* s, uint16_t from, const uint8_t* d, int len, double now);  /* true si era suyo */
void ms_seeder_update(ms_seeder_t* s, double now);
void ms_seeder_stats(const ms_seeder_t* s, ms_seeder_stats_t* st);
/* para el que todavía no es semilla: contesta "todavía no" a un pedido (true si era un pedido) */
bool ms_reply_not_ready(const ms_io_t* io, uint16_t to, const uint8_t* d, int len);

/* descarga -------------------------------------------------------------------------------------- */
typedef enum { MS_DL_MANIFEST, MS_DL_FILES, MS_DL_DONE, MS_DL_FAILED } ms_dl_state_t;
typedef struct {
    ms_dl_state_t state;
    int files_total, files_needed, files_done, files_bad;
    uint64_t bytes_needed, bytes_got;
    uint64_t bytes_served;                          /* lo que repartió mientras bajaba y después */
    int blocks_bad;
    int sources_alive, sources_busy, queue_pos;    /* queue_pos: mejor posición en una lista de espera (0 = atendido) */
    double started, finished;
    char error[128];
} ms_dl_status_t;
typedef struct ms_dl ms_dl_t;
ms_dl_t* ms_dl_new(const ms_io_t* io);
void ms_dl_free(ms_dl_t* d);
void ms_dl_start(ms_dl_t* d, uint16_t manifest_from, double now);
void ms_dl_add_source(ms_dl_t* d, uint16_t id);          /* semillas posibles (las que no contestan se descartan) */
/* repartir lo que ya se tiene (bloques verificados) mientras baja y después: como un enjambre */
void ms_dl_set_serving(ms_dl_t* d, bool on, int max_active, int chunks_per_s);
bool ms_dl_on_msg(ms_dl_t* d, uint16_t from, const uint8_t* m, int len, double now);
void ms_dl_update(ms_dl_t* d, double now);
void ms_dl_status(const ms_dl_t* d, ms_dl_status_t* st);
const ms_manifest_t* ms_dl_manifest(const ms_dl_t* d);  /* NULL hasta que llega */
#endif
