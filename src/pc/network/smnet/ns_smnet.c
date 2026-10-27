/* NetworkSystem de sm64coopdx sobre smnet (fase 4).
 *
 * Paso 2a (transporte): los paquetes de sm64coopdx viajan como mensajes de smnet "a un jugador":
 * cliente -> host y host -> cliente, por las sesiones cifradas de smnet. El host reenvía
 * (requireServerBroadcast).
 *
 * Paso 2b (estado de Mario): PACKET_PLAYER ya no pasa por el host. El paquete propio (tal como lo
 * arma sm64coopdx, comprimido) se publica como "estado rico" (blob) del flujo de estado de smnet,
 * con la posición en los 12 B de estado y el área como clave; los SN se lo entregan a los
 * MAX_PLAYERS-1 más cercanos de la misma área, y cada uno lo mete en sm64coopdx como si hubiera
 * llegado por la red. Los reenvíos de PLAYER del host se descartan (ya llegaron directo).
 *
 * Paso 2c (modo masivo, más de 16 jugadores): sm64coopdx corre en cada máquina como su propio
 * "servidor" (Host en el menú) y smnet lo une a la partida grande. Los índices locales 1..15 son
 * slots: se asignan a los más cercanos de la misma área según el flujo de estado de smnet, con
 * histéresis para que no parpadeen. El PLAYER de un jugador con slot se inyecta con el índice
 * global reescrito al slot (en el cable todos dicen ser el 0: el id real es el de smnet). Los
 * demás paquetes de sm64coopdx no salen en este modo (cada uno tiene sus enemigos y monedas);
 * los mensajes por área y globales son el paso siguiente (INTEGRACION.md §6).
 *
 * Dirección = id de smnet (u16). Paquetes de más de SMNET_MAX_APP se parten: cada salto es
 * confiable y ordenado, así que los pedazos de un mismo origen llegan en orden. */
#ifdef SMNET
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "ns_smnet.h"
#include "ns_smnet_lowpoly.h"
#include "modsync.h"
#include "ns_smnet_lobby.h"
#include "ns_smnet_mod.h"
#ifndef NS_SMNET_SIN_LUA
#include "pc/mods/mods.h"
#include "pc/mods/mod.h"
#endif
#include "pc/network/network_player.h"
#include "pc/configfile.h"
#include "pc/fs/fs.h"
#include <time.h>
#include "pc/debuglog.h"
#include "pc/djui/djui.h"
#include "pc/djui/djui_chat_message.h"
#ifndef NS_SMNET_SIN_LUA   /* la prueba del banco (juego/pruebas/slots) no tiene Lua */
#include "pc/lua/smlua.h"
#include "pc/lua/smlua_sync_table.h"
#include "pc/lua/utils/smlua_obj_utils.h"
#include "pc/lua/utils/smlua_model_utils.h"
#endif
#include "pc/network/packets/packet.h"
#include "sm64.h"
#include "game/level_update.h"
void network_send_player(u8 localIndex);   /* packet_player.c (sin declaración en un .h) */
#include "game/mario.h"
#include "game/player_palette.h"
#include "game/mario_misc.h"
#include "game/characters.h"
#include "game/camera.h"
#include "game/object_helpers.h"
#include "game/object_list_processor.h"
#include "engine/surface_collision.h"
#include "game/memory.h"
#include "mario_animation_ids.h"
#include "mario_geo_switch_case_ids.h"
#include "behavior_data.h"
#include "model_ids.h"
#include "object_fields.h"
#include "object_constants.h"
#include "pc/utils/misc.h"
#include <math.h>
#include "smnet.h"
#include "smnet_udp.h"

#define FRAG_FIRST 1
#define FRAG_LAST  2
#define FRAG_DATA  (SMNET_MAX_APP - 1)
#define PENDING_MAX 64
/* mensajes propios del modo masivo (primer byte; los del transporte clásico son 0..3) */
#define APP_CHAT    0x81
#define APP_MOD     0x82    /* mensajes de los mods (smnet_send) */
#define APP_LUA     0x83    /* paquetes de Lua de los mods (sync tables, network_send): ver lua_relay */
#define APP_LUA_REQ 0x84    /* "mandame tu gPlayerSyncTable completa" (al darle un slot) */
#define APP_BIG     0x86    /* pedazo de un mensaje grande: tipo u8, id u8, índice u8, total u8, datos */
#define APP_AREA_REQ 0x87   /* "mandame el estado de tu nivel y área" (al entrar): curso, acto, nivel s16, área */
#define APP_STATE   0x88    /* entradas como APP_LUA, respuesta a APP_AREA_REQ (no hace falta slot) */
#define APP_ALL     0x89    /* entradas como APP_LUA, para toda la partida (estrellas, guardado, gGlobalSyncTable del coordinador) */
#define APP_GLOBAL_REQ 0x8A /* "mandame gGlobalSyncTable" (al coordinador, al entrar) */
#define APP_HOST    0x8B    /* [clave 32][firma 64]: "soy el host de la partida" (firma de "SMHOST1" + id u16) */
#define APP_END     0x8C    /* el host finalizó la partida: todos se desconectan */
bool found_match(char* text);   /* packet_chat.c: filtro de palabras */

static smnet_node_t* sNode = NULL;
static int sFd = -1;
static uint16_t sIds[MAX_PLAYERS];          /* id de smnet por índice local; [0] = último origen */
static struct { uint16_t len; uint8_t* data; } sRe[SMNET_MAX_PLAYERS];   /* rearmado por origen */
static struct { uint16_t to; uint16_t len; uint8_t* data; } sPending[PENDING_MAX];
static int sNumPending = 0;
static FILE* sLog = NULL;
static double sLastStatus = 0;
static uint16_t sBlobFrom = SMNET_NO_ID;
static struct { unsigned long long tx, rx, ajeno, desconocido, grande, malo; } sPl;   /* PACKET_PLAYER por el flujo de estado */

/* ---- modo masivo: slots */
#define SLOT_SEEN_MS    1500    /* un candidato tiene que tener estado reciente */
#define SLOT_EXPIRE_MS  2500    /* un slot sin estado se libera */
#define SLOT_MIN_HOLD_MS 1000   /* un slot recién asignado no se cambia (el cambio es limpio: se puede rotar rápido) */
#define SLOT_SWAP_FACTOR 0.7f   /* se cambia solo si el nuevo está a menos del 70 % de la distancia */
#define SLOT_SWAP_MIN   400.f   /* … y por lo menos 400 unidades más cerca */
#define SLOT_SWAPS_PER_TICK 4
#define SLOT_EVERY_MS   250
#define SLOT_FULL_K     (MAX_PLAYERS - 1 + 9)   /* vecinos con estado rico (PLAYER) que manda el SN: 15 slots + 9 de margen */
static bool sMassive = false;
static bool sMassiveJoin = false;
/* ---- el host de la partida: tiene una clave propia (smnet-host.key, en la carpeta del usuario)
   que firma la lista de mods y su "soy el host". Los que se unen aprenden la clave con la lista de
   mods; si el host sale y vuelve, sigue siendo el host (gGlobalSyncTable, la lista de mods y
   "finalizar partida" son suyos) aunque la red de smnet tenga otro coordinador. */
static uint8_t sHostSk[64], sHostPub[32];   /* la propia */
static bool sHostKeyOk = false;
static uint8_t sGamePub[32], sGameSig[64], sGameManId[16];   /* la del host de la partida en la que estamos (y su firma de la lista) */
static bool sGamePubOk = false, sGameSigOk = false;
static bool sOrigHost = false;                /* este es el host de la partida (le dio Host, no Join) */
static uint16_t sHostId = SMNET_NO_ID;        /* el host verificado, si está (y no es uno mismo) */
static uint16_t sClaimedAs = SMNET_NO_ID;     /* id con el que uno ya anunció que es el host */
static double sClaimAt = 0; static int sClaimN = 0;   /* reenvíos del anuncio (se pierde si los demás todavía no conocen el id nuevo) */
static double sHostConnAt = 0;                /* cuándo entró (el host que vuelve espera la tabla global) */
static double sEndAt = 0;                     /* el host finaliza: se sale después de mandar el aviso */
static bool sEndRx = false, sEnding = false;
/* el host que vuelve: direcciones de la partida a las que probar entrar antes de abrir una nueva */
#define REJOIN_MAX 6
static smnet_addr_t sRejoin[REJOIN_MAX];
static int sRejoinN = 0, sRejoinIdx = 0;
static double sRejoinAt = 0;
static unsigned sHostPort = 0;
static struct { smnet_addr_t a; double seen; } sPeers[32];   /* de quiénes llega tráfico (para volver) */
static double sStateSaved = 0;
static struct { float x, y, z, fwd, vy; int16_t yaw; uint8_t pose, sflags, frame; double seen; } sEnt[SMNET_MAX_PLAYERS];
static double sBlobAt[SMNET_MAX_PLAYERS];   /* último PLAYER (estado rico) de cada uno: si el SN dejó de mandarlo, su slot está congelado */
static int8_t sSlotOf[SMNET_MAX_PLAYERS];          /* slot de cada id (0 = ninguno) */
static double sSlotSince[MAX_PLAYERS];
static bool sSlotReady[MAX_PLAYERS];               /* ya llegó su primer PLAYER (hasta entonces se lo sigue dibujando como impostor) */
static bool sSlotMade[MAX_PLAYERS];                /* NetworkPlayer creado (queda conectado toda la sesión) */
static struct { uint8_t len; uint8_t d[SMNET_MAX_PINFO]; } sInfo[SMNET_MAX_PLAYERS];
static uint8_t sKnown[SMNET_MAX_PLAYERS];           /* en la partida (lista de smnet) */
static int sKnownCount = 0;
/* contadores de la telemetría (telem_update) */
static struct {
    double nextAt, connAt;
    int prevStatus;
    uint32_t disc, prevFo, prevRj;
    uint64_t prevHsF, prevRelS, prevRelR, prevRx, prevTx;
    double prevAt;
} sTel;
static void lua_player_event(uint16_t id, int joined);
static void lua_mod_message(uint16_t from, const uint8_t* d, int len);
static void lua_slot_changed(int s, uint16_t id);
static void lua_relay_rx(uint16_t from, const uint8_t* d, int len, bool anyone, bool global);
static void world_big_rx(uint16_t from, const uint8_t* d, int len);
static void world_area_request_rx(uint16_t from, const uint8_t* d, int len);
static void world_area_update(double now);
static void area_state_got(void);
static void lua_relay_flush(void);
static void lua_answer_request(uint16_t from);
static void lua_answer_global(uint16_t from);
static uint16_t own_model_ext(void);
static struct { unsigned long long tx, rx, sinslot, pedidos, respuestas, malos; } sLuaRl;
static struct { unsigned long long tx, rx, sinslot, grandes, pedidos_area, respuestas_area, partidos; } sWorld;   /* paquetes del mundo */
static s32 imp_model_for_ext(uint16_t ext, u8 model);
static uint16_t info_model_ext(uint16_t id);
static double sLastSlots = 0;
static struct { unsigned long long asignados, cambios, liberados, sin_slot; } sSl;
static struct Packet sRxPacket;
static ms_seeder_t* sGameSeeder = NULL;   /* reparte los mods de la partida (ver game_mods_build) */
static double sGameModsTried = -1e9;

/* ---- modo masivo: impostores (los visibles sin slot) */
#define IMP_MAX       SMNET_RENDER_EXTRA   /* lugares extra de las tablas de dibujo de Mario */
#define IMP_MARK      0x534D0000      /* oBehParams de nuestros objetos (para reconocerlos) */
#define IMP_SEEN_MS   2500            /* los lejanos llegan a 2 Hz: con pérdidas, 1,2 s los hacía parpadear */
#define IMP_HEAD_Y    150.f           /* la gorra, a la altura de la cabeza */
#define IMP_SCALE     2.0f
/* etapa 3: niveles de detalle por puesto de distancia entre los que no tienen slot
   MEDIO   (los sImpMario más cercanos): animación completa, piso y luz en cada cuadro, sombra
   BAJO    (los sImpBajo siguientes): animación a 10 Hz (1 de cada 3 cuadros), ojos fijos, sin
           sombra (la sombra busca el piso dos veces por cuadro), piso y luz cada 3 cuadros
   LEJANO  (el resto hasta sImpMax): pose quieta mirando a su dirección, sin sombra, piso cada
           10 cuadros y luz cada 30; con COOPDX_SMNET_LEJANOS_GORRA=1, la gorra de antes
   Cambiar de nivel no recrea el objeto: solo cambian banderas (o el modelo, en el sitio). */
enum { IMP_MEDIO = 0, IMP_BAJO = 1, IMP_LEJANO = 2 };
static struct { struct Object* o; uint16_t id; bool mario; bool low; u8 model; u8 tier; uint16_t ext; float fy, fx, fz, gdy; u16 fage; struct PlayerPalette pal; } sImp[IMP_MAX];
static struct MarioState sImpMS[IMP_MAX];       /* lo mínimo que lee el dibujo de Mario */
static int sImpMario = 64;                      /* medios */
static int sImpBajo = 96;                       /* bajos */
static bool sImpGorra = false;                  /* lejanos como gorra */
static bool sImpLejCompleto = false;             /* lejanos con el modelo completo (en vez del de pocas caras) */
static bool sImpBajoCompleto = false;            /* bajos con el modelo completo (antes era así) */
static bool sImpLejQuietos = false;              /* lejanos quietos (sin animar) */
static u32 sLowId = 0;                           /* id de modelo del Mario de pocas caras (ns_smnet_lowpoly.c) */
static unsigned sImpFrame = 0;
static unsigned long long sImpTierChg = 0;
static int16_t sImpOf[SMNET_MAX_PLAYERS];      /* impostor de cada id (-1 = ninguno) */
static int sImpMax = IMP_MAX;
static s16 sImpLevel = -1, sImpArea = -1, sImpAct = -1;
static double sLastImp = 0;
static unsigned long long sImpShown = 0;

/* log propio (smnet-host.log / smnet-cliente.log en la carpeta del juego): los LOG_INFO de
   sm64coopdx solo salen con la depuración activada en la configuración */
static void smlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void smlog(const char* fmt, ...) {
    if (!sLog) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(sLog, "[%10.0f] ", smnet_now_ms());
    vfprintf(sLog, fmt, ap);
    fputc('\n', sLog);
    fflush(sLog);
    va_end(ap);
}


/* Nombre del tipo de paquete de sm64coopdx: el paquete entero va comprimido con zlib (el primer
   byte que se ve es la cabecera de zlib), así que se descomprime solo el principio. */
static const char* const kPacketNames[] = { "ACK", "PLAYER", "OBJECT", "SPAWN_OBJECTS", "SPAWN_STAR", "SPAWN_STAR_NLE", "COLLECT_STAR", "COLLECT_COIN", "COLLECT_ITEM", "GLOBAL_POPUP", "DEBUG_SYNC", "JOIN_REQUEST", "JOIN", "CHAT", "KICK", "KEEP_ALIVE", "LEAVING", "SAVE_FILE", "SAVE_SET_FLAG", "SAVE_REMOVE_FLAG", "NETWORK_PLAYERS", "DEATH", "PING", "PONG", "UNUSED_23", "CHANGE_LEVEL", "CHANGE_AREA", "LEVEL_AREA_REQUEST", "LEVEL_REQUEST", "LEVEL", "AREA_REQUEST", "AREA", "SYNC_VALID", "LEVEL_SPAWN_INFO", "LEVEL_MACRO", "LEVEL_AREA_INFORM", "LEVEL_RESPAWN_INFO", "CHANGE_WATER_LEVEL", "PLAYER_SETTINGS", "MOD_LIST_REQUEST", "MOD_LIST", "DOWNLOAD_REQUEST", "DOWNLOAD", "MOD_LIST_ENTRY", "MOD_LIST_FILE", "MOD_LIST_DONE", "LUA_SYNC_TABLE_REQUEST", "LUA_SYNC_TABLE", "NETWORK_PLAYERS_REQUEST", "REQUEST_FAILED", "LUA_CUSTOM", "LUA_CUSTOM_BYTESTRING", "COMMAND", "MODERATOR" };
/* descomprime solo los primeros `cap` bytes; devuelve cuántos salieron (o -1) */
static int peek(const uint8_t* d, int len, uint8_t* out, int cap) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit(&z) != Z_OK) return -1;
    z.next_in = (Bytef*)d; z.avail_in = (uInt)len;
    z.next_out = out; z.avail_out = (uInt)cap;
    int rc = inflate(&z, Z_SYNC_FLUSH);
    int got = cap - (int)z.avail_out;
    inflateEnd(&z);
    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) return -1;
    return got;
}

static const char* pkt_name(const uint8_t* d, int len) {
    static char buf[48];
    uint8_t out[4] = { 0 };
    if (peek(d, len, out, sizeof out) < 1) return "?(sin comprimir)";
    unsigned t = out[0];
    if (t < sizeof kPacketNames / sizeof kPacketNames[0]) snprintf(buf, sizeof buf, "%s", kPacketNames[t]);
    else snprintf(buf, sizeof buf, "tipo %u", t);
    return buf;
}

static uint16_t area_key(unsigned level, unsigned area, unsigned act) {
    return (uint16_t)(((level & 0x3FF) << 6) | ((area & 7) << 3) | (act & 7));
}

/* PACKET_PLAYER sin comprimir: tipo, seq (2), flags, destino, [ordenado: 5 B], [nivel+área: 5 B
   o nivel: 4 B] y después el índice global del jugador (packet_read_write.c, packet_player.c).
   Devuelve 1 si es un PLAYER y completa el índice global y la clave de área de smnet. */
static int player_pkt_info(const uint8_t* d, int len, uint8_t* globalIndex, uint16_t* areaKey) {
    uint8_t b[32];
    int got = peek(d, len, b, sizeof b);
    if (got < 6 || b[0] != PACKET_PLAYER) return 0;
    uint8_t flags = b[3];
    int off = 5;
    unsigned level = 0, area = 0, act = 0;
    if (flags & 4) off += 5;
    if (flags & 1) {
        if (got < off + 5) return 0;
        act = b[off + 1]; level = (unsigned)(b[off + 2] | (b[off + 3] << 8)); area = b[off + 4];
        off += 5;
    } else if (flags & 8) {
        if (got < off + 4) return 0;
        act = b[off + 1]; level = (unsigned)(b[off + 2] | (b[off + 3] << 8));
        off += 4;
    }
    if (got <= off) return 0;
    *globalIndex = b[off];
    /* nivel (10 bits), área (3) y acto (3): sin choques en los niveles del juego */
    *areaKey = area_key(level, area, act);
    return 1;
}

/* el PLAYER propio: los 12 B de estado salen de Mario (posición para el interés y, después, para
   los impostores) y el paquete entero va como blob */
/* formato 1 del byte "action" del estado (versiones anteriores, todavía se entiende): bit 7 = en
   el aire; el resto, qué salto es */
enum { POSE_OTRO = 0, POSE_SALTO, POSE_DOBLE, POSE_TRIPLE, POSE_MORTAL_ATRAS, POSE_MORTAL_LADO, POSE_SALTO_LARGO,
       POSE_CAIDA, POSE_PATADA_PARED, POSE_GOLPE_SUELO, POSE_CLAVADO, POSE_GIRO, POSE_AIRE = 0x80 };

/* Estado de 12 B, formato 2 (bit STF_V2 de flags): "action" es la animación que tiene puesta
   (CharacterAnimID, la misma para todos los personajes) y "anim_frame" su cuadro; así el impostor
   muestra exactamente lo que hace (patadas, clavados, deslizarse, nadar, trepar...). flags (4 bits):
   STF_AIRE en el aire (se adelanta con gravedad), STF_ALTURA no está apoyado en el piso (aire, agua,
   colgado, en un poste, arriba de algo): el impostor usa su altura en vez de apoyarlo en el piso. */
#define STF_AIRE   0x01
#define STF_ALTURA 0x02
#define STF_V2     0x08
#define ANIM_NONE  0xFF
static uint8_t anim_code(const struct MarioState* m) {
    if (m->marioObj == NULL) return ANIM_NONE;
    s32 id = m->marioObj->header.gfx.animInfo.animID;
    const struct Character* ch = m->character ? m->character : &gCharacters[CT_MARIO];
    /* la animación propia del personaje (mods) se traduce a la genérica */
    if (id >= CHAR_ANIM_MAX) {
        for (s32 c = 0; c < CHAR_ANIM_MAX; c++) if (ch->anims[c] == id) return (uint8_t)c;
        return ANIM_NONE;
    }
    return id < 0 || id >= ANIM_NONE ? ANIM_NONE : (uint8_t)id;
}
static void state_fill_pose(const struct MarioState* m, smnet_state_t* st) {
    u32 a = m->action;
    st->action = anim_code(m);
    s32 fr = m->marioObj ? m->marioObj->header.gfx.animInfo.animFrame : 0;
    st->anim_frame = (uint8_t)(fr < 0 ? 0 : fr > 255 ? 255 : fr);
    st->flags = STF_V2;
    if (a & ACT_FLAG_AIR) st->flags |= STF_AIRE;
    if ((a & (ACT_FLAG_AIR | ACT_FLAG_SWIMMING | ACT_FLAG_METAL_WATER | ACT_FLAG_ON_POLE | ACT_FLAG_HANGING)) ||
        m->pos[1] > m->floorHeight + 20.f) st->flags |= STF_ALTURA;
}

static void publish_player(uint16_t areaKey, const uint8_t* d, int len) {
    if (len > SMNET_MAX_BLOB) { sPl.grande++; return; }
    smnet_state_t st;
    memset(&st, 0, sizeof st);
    struct MarioState* m = &gMarioStates[0];
    if (m->marioObj != NULL) {
        st.x = m->pos[0]; st.y = m->pos[1]; st.z = m->pos[2];
        st.yaw = m->faceAngle[1];
        st.fwd_vel = m->forwardVel;
        st.vel_y = m->vel[1];
        state_fill_pose(m, &st);
    }
    smnet_node_set_local(sNode, areaKey, &st, d, len);
    sPl.tx++;
}

/* ---------------------------------------------------------------- modo masivo */

bool ns_smnet_massive(void) { return sMassive && sNode != NULL; }

/* Join por IP en modo masivo: se guarda el destino y se arranca como Host */
static char sReqJoin[96];
static char sJoinTarget[96];
static int sJoinState = 0;          /* 0 nada, 1 conectando, 2 conectado, 3 falló (se avisó) */
static double sJoinSince = 0;
static smnet_addr_t sJoinAddr;      /* a dónde se unió (para volver a entrar de cero) */
static bool sJoinAddrOk = false;
static double sLostAt = 0;          /* desde cuándo está sin conexión (0 = conectado) */
static void rejoin_fresh(double now);
bool ns_smnet_massive_default(void) {
    const char* mas = getenv("COOPDX_SMNET_MASIVO");
    return !(mas && mas[0] == '0');
}
void ns_smnet_request_massive_join(const char* ip, unsigned port) {
    if (port == 0) port = DEFAULT_PORT;
    snprintf(sReqJoin, sizeof sReqJoin, "%s:%u", (ip && ip[0]) ? ip : "127.0.0.1", port);
}

/* avisos en el chat del estado de la unión (el que se une ya está jugando en su mundo) */
static void join_status_update(double now) {
    if (sJoinState == 0 || sJoinState >= 2) {
        if (sJoinState == 2) {
            smnet_node_info_t inf; smnet_node_info(sNode, &inf);
            if (inf.status != SMNET_PEER_CONNECTED) { sJoinState = 1; sJoinSince = now; sLostAt = now; djui_chat_message_create("\\#ffc080\\smnet: se perdió la conexión, reintentando…"); }
        }
        return;
    }
    /* sin conexión hace rato (la partida chica se cae si sale el host: nadie toma el control): se
       vuelve a entrar de cero a la dirección del Join; si el host vuelve, se entra a la suya */
    if (sLostAt > 0 && now - sLostAt > 8000 && sMassiveJoin && sJoinAddrOk) { sLostAt = now; rejoin_fresh(now); return; }
    smnet_node_info_t inf; smnet_node_info(sNode, &inf);
    if (inf.status == SMNET_PEER_CONNECTED) {
        char m[200];
        snprintf(m, sizeof m, "\\#a0ffa0\\smnet: conectado a la partida de %s (%d jugadores)", sJoinTarget, sKnownCount + 1);
        djui_chat_message_create(m);
        sJoinState = 2; sLostAt = 0;
    } else if (now - sJoinSince > 10000 && sJoinState == 1) {
        char m[200];
        snprintf(m, sizeof m, "\\#ff8080\\smnet: no responde %s; se sigue intentando (mientras, jugás solo)", sJoinTarget);
        djui_chat_message_create(m);
        sJoinSince = now + 50000;   /* el próximo aviso, en un minuto */
    }
}

/* identidad de smnet de esta instalación (smnet-identidad.key, en la carpeta del usuario): la usa
   el coordinador para los baneos. Dos instancias en la misma PC la comparten (cada una tiene su
   número de sesión), así que un baneo las alcanza a las dos: para probar en una sola PC, las
   instancias que se unen usan COOPDX_SMNET_IDENTIDAD_NUEVA=1 (jugar-local.sh y abrir-instancias.sh). */
static void identity_seed_load(uint8_t out[32]) {
    /* COOPDX_SMNET_IDENTIDAD_NUEVA=1: una al azar (para probar con varias instancias en la misma PC) */
    const char* e = getenv("COOPDX_SMNET_IDENTIDAD_NUEVA");
    if (e && e[0] == '1') { memset(out, 0, 32); return; }
    const char* path = fs_get_write_path("smnet-identidad.key");
    FILE* f = fopen(path, "rb");
    bool ok = f && fread(out, 1, 32, f) == 32;
    if (f) fclose(f);
    if (ok) return;
    if (smnet_random(out, 32) != 0) { memset(out, 0, 32); return; }
    f = fopen(path, "wb");
    if (f) { fwrite(out, 1, 32, f); fclose(f); }
}

/* ---- el host de la partida (ver sOrigHost) */
static void host_key_load(void) {
    if (sHostKeyOk) return;
    const char* path = fs_get_write_path("smnet-host.key");
    uint8_t seed[32];
    FILE* f = fopen(path, "rb");
    bool ok = f && fread(seed, 1, 32, f) == 32;
    if (f) fclose(f);
    if (!ok) {
        if (smnet_random(seed, 32) != 0) return;
        f = fopen(path, "wb");
        if (f) { fwrite(seed, 1, 32, f); fclose(f); }
    }
    ms_key_pair(sHostSk, sHostPub, seed);
    memset(seed, 0, sizeof seed);
    sHostKeyOk = true;
}
void ns_smnet_set_game_host(const uint8_t pub[32], const uint8_t sig[64], const uint8_t manId[16]) {
    sGamePubOk = pub != NULL; sGameSigOk = sig != NULL && manId != NULL;
    if (pub) memcpy(sGamePub, pub, 32);
    if (sig) memcpy(sGameSig, sig, 64);
    if (manId) memcpy(sGameManId, manId, 16);
}
static void host_claim_msg(uint16_t id, uint8_t out[9]) { memcpy(out, "SMHOST1", 7); memcpy(out + 7, &id, 2); }
static void host_claim_send(smnet_scope_t scope, uint16_t to) {
    smnet_node_info_t inf; smnet_node_info(sNode, &inf);
    if (!sOrigHost || !sHostKeyOk || inf.id == SMNET_NO_ID) return;
    uint8_t b[1 + 32 + 64], m[9];
    b[0] = APP_HOST; memcpy(b + 1, sHostPub, 32);
    host_claim_msg(inf.id, m);
    ms_sign(b + 33, sHostSk, m, 9);
    smnet_node_send_msg(sNode, scope, to, b, sizeof b);
}
static void info_name(uint16_t id, char* out, int cap);
static void host_claim_rx(uint16_t from, const uint8_t* d, int len) {
    if (len < 96 || sOrigHost) return;
    uint8_t m[9]; host_claim_msg(from, m);
    if (ms_check(d + 32, d, m, 9) != 0) { smlog("smnet:%u dice ser el host con una firma que no vale", from); return; }
    if (sGamePubOk && memcmp(d, sGamePub, 32) != 0) { smlog("smnet:%u dice ser el host con otra clave: no se le cree", from); return; }
    if (!sGamePubOk) { memcpy(sGamePub, d, 32); sGamePubOk = true; }
    if (sHostId == from) return;
    bool back = sHostId == SMNET_NO_ID && sJoinState == 2 && smnet_now_ms() - sJoinSince > 5000;
    sHostId = from;
    char nm[40]; info_name(from, nm, sizeof nm);
    smlog("host de la partida: smnet:%u (%s)", from, nm);
    if (back) { char t[120]; snprintf(t, sizeof t, "\\#a0ffa0\\%s (el host) volvió a la partida", nm); djui_chat_message_create(t); }
}
#ifndef NS_SMNET_SIN_LUA
/* quién manda gGlobalSyncTable: el host si está; si no, el coordinador de smnet */
static bool i_am_coord(void);
static uint16_t coord_id(void);
static bool host_ready(void) { return sOrigHost && sHostConnAt > 0 && smnet_now_ms() - sHostConnAt > 3000; }
static bool i_am_global_authority(void) {
    if (sOrigHost) {   /* el que vuelve espera a tener la tabla de la partida antes de mandar */
        smnet_node_info_t inf; smnet_node_info(sNode, &inf);
        return inf.id == inf.coord_id || host_ready();
    }
    if (sHostId != SMNET_NO_ID) return false;
    return i_am_coord();
}
static uint16_t global_authority_id(void) {
    if (sOrigHost) return coord_id();          /* el host acepta la del coordinador al volver */
    return sHostId != SMNET_NO_ID ? sHostId : coord_id();
}
#endif

/* partida guardada del host (smnet-partida-host.txt): a quiénes volver a entrar si le da Host de
   nuevo. Se escribe cada 10 s mientras hostea y al salir; "finalizar partida" la borra. */
#define REJOIN_TTL_S (12 * 3600)
static void peers_note(const smnet_addr_t* a, double now) {
    int free_i = -1, old_i = 0;
    for (int i = 0; i < 32; i++) {
        if (sPeers[i].seen > 0 && smnet_addr_eq(&sPeers[i].a, a)) { sPeers[i].seen = now; return; }
        if (sPeers[i].seen <= 0 && free_i < 0) free_i = i;
        if (sPeers[i].seen < sPeers[old_i].seen) old_i = i;
    }
    int k = free_i >= 0 ? free_i : old_i;
    sPeers[k].a = *a; sPeers[k].seen = now;
}
static void game_state_save(double now) {
    if (!sOrigHost || !sMassive || !sNode) return;
    smnet_node_info_t inf; smnet_node_info(sNode, &inf);
    if (inf.status != SMNET_PEER_CONNECTED) return;
    int n = 0;
    for (int i = 0; i < 32; i++) if (sPeers[i].seen > 0 && now - sPeers[i].seen < 30000) n++;
    if (n == 0) { remove(fs_get_write_path("smnet-partida-host.txt")); return; }   /* solo: no hay a quién volver */
    FILE* f = fopen(fs_get_write_path("smnet-partida-host.txt"), "w");
    if (!f) return;
    fprintf(f, "v1 %lld\n", (long long)time(NULL));
    /* primero los que se oyeron hace menos */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < 32; i++) {
            if (sPeers[i].seen <= 0 || now - sPeers[i].seen >= 30000) continue;
            if ((pass == 0) != (now - sPeers[i].seen < 3000)) continue;
            char hx[64]; ms_hex(sPeers[i].a.data, sPeers[i].a.len, hx);
            fprintf(f, "%s\n", hx);
        }
    fclose(f);
}
static int game_state_load(smnet_addr_t* out, int cap) {
    FILE* f = fopen(fs_get_write_path("smnet-partida-host.txt"), "r");
    if (!f) return 0;
    char line[128]; long long t = 0; int n = 0;
    if (!fgets(line, sizeof line, f) || sscanf(line, "v1 %lld", &t) != 1 || (long long)time(NULL) - t > REJOIN_TTL_S) { fclose(f); return 0; }
    while (n < cap && fgets(line, sizeof line, f)) {
        int L = 0; while (line[L] && line[L] != '\n' && line[L] != '\r') L++;
        if (L < 2 || L / 2 > (int)sizeof out[n].data) continue;
        memset(&out[n], 0, sizeof out[n]);
        bool ok = true;
        for (int i = 0; i < L / 2; i++) { unsigned v; if (sscanf(line + 2 * i, "%2x", &v) != 1) { ok = false; break; } out[n].data[i] = (uint8_t)v; }
        if (!ok) continue;
        out[n].len = (uint8_t)(L / 2);
        n++;
    }
    fclose(f);
    return n;
}

/* pausa (djui_panel_pause.c) */
bool ns_smnet_is_game_host(void) { return ns_smnet_massive() && sOrigHost; }
bool ns_smnet_joined_game(void) { return ns_smnet_massive() && !sOrigHost; }
/* para ns_smnet_mod.c */
struct smnet_node* ns_smnet_node_ptr(void) { return sMassive ? sNode : NULL; }
uint16_t ns_smnet_host_smnet_id(void) { return sHostId; }
bool ns_smnet_is_orig_host_now(void) { return sOrigHost; }
uint16_t ns_smnet_self_smnet_id(void) { if (!sNode) return SMNET_NO_ID; smnet_node_info_t inf; smnet_node_info(sNode, &inf); return inf.id; }
bool ns_smnet_known_player(uint16_t id) { return id < SMNET_MAX_PLAYERS && sKnown[id] && !ns_smnet_mod_hidden(id); }
static void info_name(uint16_t id, char* out, int cap);
void ns_smnet_player_name(uint16_t id, char* out, int cap) { if (id >= SMNET_MAX_PLAYERS) { snprintf(out, (size_t)cap, "?"); return; } info_name(id, out, cap); }
void ns_smnet_end_game(void) {
    if (!ns_smnet_is_game_host()) return;
    uint8_t b = APP_END;
    smnet_node_send_msg(sNode, SMNET_TO_ALL, 0, &b, 1);
    sEnding = true;
    remove(fs_get_write_path("smnet-partida-host.txt"));
    sEndAt = smnet_now_ms() + 700;   /* que salga el aviso antes de cerrar */
    smlog("el host finaliza la partida");
}

/* propiedad de los objetos (sync_object.c): otro jugador que simula su mundo en la misma área
   (aunque no tenga slot acá) está más cerca del objeto: es suyo. Se arma la lista una vez por cuadro. */
#define OWN_CAND_MAX 512
static struct { float x, y, z; } sOwnCand[OWN_CAND_MAX];
static int sOwnCandN = 0;
static u32 sOwnCandFrame = 0xFFFFFFFFu;
static bool info_location(uint16_t id, s16* level, u8* area, u8* act);
bool ns_smnet_other_owner_closer(f32 x, f32 y, f32 z, f32 myDist) {
    if (!ns_smnet_massive()) return false;
    extern u32 gGlobalTimer;
    if (sOwnCandFrame != gGlobalTimer) {
        extern s16 gCurrActStarNum, gCurrLevelNum, gCurrAreaIndex;
        sOwnCandFrame = gGlobalTimer; sOwnCandN = 0;
        double now = smnet_now_ms();
        smnet_node_info_t inf; smnet_node_info(sNode, &inf);
        for (int id = 0; id < SMNET_MAX_PLAYERS && sOwnCandN < OWN_CAND_MAX; id++) {
            if (!sKnown[id] || id == inf.id || sSlotOf[id] > 0) continue;
            if (sEnt[id].seen <= 0 || now - sEnt[id].seen > 1000) continue;
            s16 lv; u8 ar, ac;
            if (!info_location((uint16_t)id, &lv, &ar, &ac)) continue;   /* bots: no simulan */
            if (lv != gCurrLevelNum || ar != (u8)gCurrAreaIndex || ac != (u8)gCurrActStarNum) continue;
            sOwnCand[sOwnCandN].x = sEnt[id].x; sOwnCand[sOwnCandN].y = sEnt[id].y; sOwnCand[sOwnCandN].z = sEnt[id].z;
            sOwnCandN++;
        }
    }
    float d2 = myDist * myDist;
    for (int i = 0; i < sOwnCandN; i++) {
        float dx = sOwnCand[i].x - x, dy = sOwnCand[i].y - y, dz = sOwnCand[i].z - z;
        if (dx * dx + dy * dy + dz * dz < d2) return true;
    }
    return false;
}

/* datos de presentación propios (los reparte smnet a todos): modelo, paleta, nombre */
static void publish_info(void) {
    uint8_t b[SMNET_MAX_PINFO];
    int n = 0;
    b[n++] = (uint8_t)configPlayerModel;
    memcpy(b + n, &configPlayerPalette, sizeof(struct PlayerPalette)); n += (int)sizeof(struct PlayerPalette);
    /* nombre, un 0 y el modelo extendido (u16) que tiene puesto Mario: lo cambian los mods
       como Character Select; los impostores lo usan. Quien lee solo el nombre corta en el 0. */
    /* … y dónde está (nivel s16, área u8, acto u8) para la lista global de jugadores */
    int nl = (int)strlen(configPlayerName);
    if (nl > SMNET_MAX_PINFO - n - 7) nl = SMNET_MAX_PINFO - n - 7;
    memcpy(b + n, configPlayerName, (size_t)nl); n += nl;
    b[n++] = 0;
    uint16_t ext = own_model_ext();
    memcpy(b + n, &ext, 2); n += 2;
    { extern s16 gCurrActStarNum, gCurrLevelNum, gCurrAreaIndex;
      s16 lv = gCurrLevelNum; memcpy(b + n, &lv, 2); n += 2;
      b[n++] = (uint8_t)gCurrAreaIndex; b[n++] = (uint8_t)gCurrActStarNum; }
    smnet_node_set_player_info(sNode, b, n);
}

/* nombre, cambiado en el menú (packet_player_settings.c) o el modelo/paleta: se vuelve a anunciar */
static bool info_location(uint16_t id, s16* level, u8* area, u8* act);
void ns_smnet_settings_changed(void) { if (ns_smnet_massive()) publish_info(); }

/* ¿el que ocupa este slot simula su mundo? (los bots no: sus datos no traen el final con modelo y
   ubicación) — para no darles la propiedad de los objetos */
bool ns_smnet_slot_simulates(u8 localIndex) {
    if (!ns_smnet_massive() || localIndex == 0 || localIndex >= MAX_PLAYERS) return true;
    uint16_t id = sIds[localIndex];
    if (id == SMNET_NO_ID || id >= SMNET_MAX_PLAYERS) return false;
    s16 lv; u8 ar, ac;
    return info_location(id, &lv, &ar, &ac);
}
/* id de smnet propio (bloques de ids de objetos creados en el momento) o -1 */
s32 ns_smnet_local_smnet_id(void) {
    if (!ns_smnet_massive()) return -1;
    smnet_node_info_t inf; smnet_node_info(sNode, &inf);
    return inf.id == SMNET_NO_ID ? -1 : (s32)inf.id;
}

/* dónde está un jugador según sus datos de presentación (false si no lo dice) */
static bool info_location(uint16_t id, s16* level, u8* area, u8* act) {
    int n = sInfo[id].len, off = 1 + (int)sizeof(struct PlayerPalette);
    const uint8_t* d = sInfo[id].d;
    while (off < n && d[off] != 0) off++;
    if (off + 7 > n) return false;
    memcpy(level, d + off + 3, 2); *area = d[off + 5]; *act = d[off + 6];
    return true;
}

/* ---- lista global de jugadores (TAB en modo masivo): uno mismo, los de la misma área del más
   cercano al más lejano y después el resto por nombre. Se rearma cada medio segundo. */
static uint16_t sList[SMNET_MAX_PLAYERS + 1];
static int sListN = 0;
static double sListAt = -1e9;
static void info_name(uint16_t id, char* out, int cap) {
    int n = sInfo[id].len, off = 1 + (int)sizeof(struct PlayerPalette), k = 0;
    const uint8_t* d = sInfo[id].d;
    while (off < n && d[off] != 0 && k < cap - 1) out[k++] = (char)d[off++];
    out[k] = 0;
    if (k == 0) snprintf(out, (size_t)cap, "Jugador%u", (unsigned)id);
}
static float sListKey[SMNET_MAX_PLAYERS];
static char sListName[SMNET_MAX_PLAYERS][24];
static int list_cmp(const void* a, const void* b) {
    uint16_t x = *(const uint16_t*)a, y = *(const uint16_t*)b;
    if (sListKey[x] != sListKey[y]) return sListKey[x] < sListKey[y] ? -1 : 1;
    int c = strcmp(sListName[x], sListName[y]);
    return c ? c : (int)x - (int)y;
}
static void list_rebuild(void) {
    double now = smnet_now_ms();
    if (now - sListAt < 500) return;
    sListAt = now;
    smnet_node_info_t inf; smnet_node_info(sNode, &inf);
    struct MarioState* m = &gMarioStates[0];
    sListN = 0;
    sList[sListN++] = SMNET_NO_ID;          /* uno mismo */
    int k0 = sListN;
    for (int id = 0; id < SMNET_MAX_PLAYERS; id++) {
        if (!sKnown[id] || id == inf.id || ns_smnet_mod_hidden(id)) continue;
        info_name((uint16_t)id, sListName[id], (int)sizeof sListName[id]);
        bool here = sEnt[id].seen > 0 && now - sEnt[id].seen < 3000;
        float dx = sEnt[id].x - m->pos[0], dy = sEnt[id].y - m->pos[1], dz = sEnt[id].z - m->pos[2];
        sListKey[id] = here ? sqrtf(dx * dx + dy * dy + dz * dz) : 1e30f;
        sList[sListN++] = (uint16_t)id;
    }
    qsort(sList + k0, (size_t)(sListN - k0), sizeof sList[0], list_cmp);
}
int ns_smnet_list_count(void) { if (!ns_smnet_massive()) return 0; list_rebuild(); return sListN; }
bool ns_smnet_list_get(int k, struct NsSmnetListEntry* e) {
    if (!ns_smnet_massive() || k < 0 || k >= sListN) return false;
    memset(e, 0, sizeof *e);
    uint16_t id = sList[k];
    if (id == SMNET_NO_ID) {
        extern s16 gCurrActStarNum, gCurrLevelNum, gCurrAreaIndex;
        e->self = true;
        e->localIndex = 0;
        snprintf(e->name, sizeof e->name, "%s", configPlayerName);
        e->model = (u8)(configPlayerModel < CT_MAX ? configPlayerModel : 0);
        memcpy(e->rgb, configPlayerPalette.parts[CAP], 3);
        e->level = gCurrLevelNum; e->area = (u8)gCurrAreaIndex; e->act = (u8)gCurrActStarNum; e->hasLocation = true;
        return true;
    }
    info_name(id, e->name, (int)sizeof e->name);
    for (char* c = e->name; *c; c++) if (*c == ' ') *c = '_';
    if (sInfo[id].len >= 1 + (int)sizeof(struct PlayerPalette)) {
        e->model = sInfo[id].d[0] < CT_MAX ? sInfo[id].d[0] : 0;
        const struct PlayerPalette* pal = (const struct PlayerPalette*)(sInfo[id].d + 1);
        memcpy(e->rgb, pal->parts[CAP], 3);
    } else { e->rgb[0] = e->rgb[1] = e->rgb[2] = 220; }
    e->slot = sSlotOf[id] > 0;
    e->localIndex = e->slot ? sSlotOf[id] : -1;
    e->hasLocation = info_location(id, &e->level, &e->area, &e->act);
    return true;
}

/* modelo extendido anunciado por un jugador (0 = ninguno) */
static uint16_t info_model_ext(uint16_t id) {
    int n = sInfo[id].len, off = 1 + (int)sizeof(struct PlayerPalette);
    const uint8_t* d = sInfo[id].d;
    while (off < n && d[off] != 0) off++;
    if (off + 3 > n) return 0;
    uint16_t ext; memcpy(&ext, d + off + 1, 2);
    return ext;
}

static void slot_apply_info(int s, uint16_t id) {
    struct PlayerPalette pal;
    memcpy(&pal, &configPlayerPalette, sizeof pal);
    u8 model = 0;
    char name[MAX_CONFIG_STRING];
    snprintf(name, sizeof name, "Jugador%u", (unsigned)id);
    int n = sInfo[id].len;
    const uint8_t* d = sInfo[id].d;
    if (n >= 1 + (int)sizeof pal) {
        model = d[0];
        memcpy(&pal, d + 1, sizeof pal);
        int nl = n - 1 - (int)sizeof pal;
        if (nl > 0) {
            if (nl > MAX_CONFIG_STRING - 1) nl = MAX_CONFIG_STRING - 1;
            memcpy(name, d + 1 + sizeof pal, (size_t)nl);
            name[nl] = '\0';
        }
    }
    /* sm64coopdx no acepta espacios en el nombre (pone "Player"): se cambian por _ */
    for (char* c = name; *c; c++) if (*c == ' ') *c = '_';
    /* si el slot estaba libre se crea el NetworkPlayer (sin cartel de "se conectó"); si estaba
       ocupado por otro (cambio por histéresis) solo se le cambian nombre, modelo y paleta */
    extern bool gCurrentlyJoining;
    bool was = gCurrentlyJoining;
    gCurrentlyJoining = true;
    network_player_connected(NPT_CLIENT, (u8)s, model, &pal, name, "");
    gCurrentlyJoining = was;
    sSlotMade[s] = true;
}

/* slot sin jugador: invisible (fuera de cualquier área) */
static void slot_park(int s) {
    struct NetworkPlayer* np = &gNetworkPlayers[s];
    np->currCourseNum = -1; np->currActNum = -1; np->currLevelNum = -1; np->currAreaIndex = -1;
    np->currPositionValid = false;
    np->currLevelSyncValid = false;
    np->currAreaSyncValid = false;
    np->fadeOpacity = 0;
    sSlotReady[s] = false;
    sLastImp = 0;   /* el que sale del slot pasa a impostor en este mismo cuadro */
}

static void slot_assign(int s, uint16_t id, double now) {
    uint16_t old = sIds[s];
    if (old != SMNET_NO_ID && old < SMNET_MAX_PLAYERS) sSlotOf[old] = 0;
    sIds[s] = id;
    sSlotOf[id] = (int8_t)s;
    sSlotSince[s] = now;
    slot_park(s);                 /* hasta que llegue su PLAYER */
    slot_apply_info(s, id);
    init_mario_single_from_save_file(&gMarioStates[s], (u16)s);
    slot_park(s);
    lua_slot_changed(s, id);
    if (old != SMNET_NO_ID) sSl.cambios++; else sSl.asignados++;
    smlog("slot %d: smnet:%u%s", s, id, old != SMNET_NO_ID ? " (reemplaza a otro)" : "");
}

static void slot_free(int s) {
    uint16_t old = sIds[s];
    if (old != SMNET_NO_ID && old < SMNET_MAX_PLAYERS) sSlotOf[old] = 0;
    sIds[s] = SMNET_NO_ID;
    if (sSlotMade[s]) {
        /* fuera de la lista de jugadores (TAB) hasta que el slot se vuelva a usar; sin
           network_player_disconnected: no hay cartel de "se desconectó" (solo se fue del área) */
        slot_park(s);
        lua_slot_changed(s, SMNET_NO_ID);
        gNetworkPlayers[s].connected = false;
        sSlotMade[s] = false;
    }
    sSl.liberados++;
    smlog("slot %d libre (era smnet:%u)", s, old);
}

static void slots_update(double now) {
    smnet_node_info_t inf;
    smnet_node_info(sNode, &inf);
    struct MarioState* m = &gMarioStates[0];
    float ox = m->pos[0], oy = m->pos[1], oz = m->pos[2];
    /* mantener vivos los NetworkPlayer virtuales (el "servidor" local los daría por caídos) */
    for (int s = 1; s < MAX_PLAYERS; s++) if (sSlotMade[s]) gNetworkPlayers[s].lastReceived = clock_elapsed();
    if (now - sLastSlots < SLOT_EVERY_MS) return;
    sLastSlots = now;
    /* liberar los que dejaron de llegar (se fueron del área o de la partida) */
    for (int s = 1; s < MAX_PLAYERS; s++) {
        uint16_t id = sIds[s];
        if (id != SMNET_NO_ID && (id >= SMNET_MAX_PLAYERS || now - sEnt[id].seen > SLOT_EXPIRE_MS)) slot_free(s);
    }
    /* candidatos sin slot, del más cercano al más lejano */
    static uint16_t cand[SMNET_MAX_PLAYERS];
    static float cd[SMNET_MAX_PLAYERS];
    int nc = 0;
    for (int id = 0; id < SMNET_MAX_PLAYERS; id++) {
        if (id == inf.id || sSlotOf[id] || sEnt[id].seen <= 0 || now - sEnt[id].seen > SLOT_SEEN_MS) continue;
        float dx = sEnt[id].x - ox, dy = sEnt[id].y - oy, dz = sEnt[id].z - oz;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        int k = nc++;
        while (k > 0 && cd[k - 1] > d) { cand[k] = cand[k - 1]; cd[k] = cd[k - 1]; k--; }
        cand[k] = (uint16_t)id; cd[k] = d;
    }
    sSl.sin_slot = (unsigned long long)nc;
    int ci = 0;
    /* llenar los libres */
    for (int s = 1; s < MAX_PLAYERS && ci < nc; s++) if (sIds[s] == SMNET_NO_ID) slot_assign(s, cand[ci++], now);
    /* histéresis: se cambia el slot más lejano por el candidato más cercano solo si este está
       claramente más cerca (55 % de la distancia y 600 u). Excepción: si el del slot quedó fuera
       de los SLOT_FULL_K más cercanos, el SN ya no le manda su PLAYER (se vería congelado) y se
       cambia igual. A lo sumo SLOT_SWAPS_PER_TICK cambios por vuelta. */
    for (int round = 0; round < SLOT_SWAPS_PER_TICK && ci < nc; round++) {
        int worst = -1;
        float wd = -1;
        for (int s = 1; s < MAX_PLAYERS; s++) {
            uint16_t id = sIds[s];
            if (id == SMNET_NO_ID || now - sSlotSince[s] < SLOT_MIN_HOLD_MS) continue;
            float dx = sEnt[id].x - ox, dy = sEnt[id].y - oy, dz = sEnt[id].z - oz;
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > wd) { wd = d; worst = s; }
        }
        if (worst <= 0) break;
        int rank = 0;   /* cuántos visibles están más cerca que él */
        for (int s = 1; s < MAX_PLAYERS; s++) {
            uint16_t id = sIds[s];
            if (id == SMNET_NO_ID || s == worst) continue;
            float dx = sEnt[id].x - ox, dy = sEnt[id].y - oy, dz = sEnt[id].z - oz;
            if (sqrtf(dx * dx + dy * dy + dz * dz) < wd) rank++;
        }
        for (int j = ci; j < nc && cd[j] < wd; j++) rank++;
        bool clearly = cd[ci] < wd * SLOT_SWAP_FACTOR && wd - cd[ci] > SLOT_SWAP_MIN;
        /* congelado: el SN ya no le manda su PLAYER (quedó fuera de sus vecinos ricos). Se mira lo
           que llega de verdad y no un ranking propio: con cientos amontonados, el ranking de uno y
           el del SN no coinciden y los slots rotaban sin parar (parpadeos, pieles cambiadas) */
        uint16_t wid = sIds[worst];
        bool starved = cd[ci] < wd && ((wid < SMNET_MAX_PLAYERS && now - sBlobAt[wid] > 800.0) || rank >= SLOT_FULL_K + 8);
        if (!clearly && !starved) break;
        slot_assign(worst, cand[ci], now);
        ci++;
    }
    sSl.sin_slot = (unsigned long long)(nc - ci);
}

/* spawn_object guarda el comportamiento ya traducido; se compara contra lo mismo */
static const BehaviorScript* smnet_static_bhv(void) { static const BehaviorScript* b = NULL; if (!b) b = (const BehaviorScript*)segmented_to_virtual(bhvStaticObject); return b; }

static bool imp_alive(int i) {
    struct Object* o = sImp[i].o;
    return o != NULL && o->activeFlags != ACTIVE_FLAG_DEACTIVATED && o->behavior == smnet_static_bhv() &&
           o->oBehParams == (s32)(IMP_MARK | (u32)i);
}

static void imp_drop(int i, bool delete_obj) {
    if (delete_obj && imp_alive(i)) obj_mark_for_deletion(sImp[i].o);
    if (sImp[i].id < SMNET_MAX_PLAYERS) sImpOf[sImp[i].id] = -1;
    sImp[i].o = NULL;
    sImp[i].id = SMNET_NO_ID;
    sImp[i].mario = false;
    sImp[i].low = false;
    sImp[i].tier = IMP_MEDIO;
}

static void imp_forget_all(bool delete_obj) {
    for (int i = 0; i < IMP_MAX; i++) if (sImp[i].o || sImp[i].id != SMNET_NO_ID) imp_drop(i, delete_obj);
}

/* Animación propia de cada impostor Mario (etapa 2). En sm64coopdx todas las animaciones de
   Mario están en memoria (gMarioAnims): load_patchable_table solo apunta targetAnim a la que
   corresponde, así que cada impostor tiene la suya sin copiar nada. Se elige por la velocidad y
   la vertical de los 12 B de estado (quieto, caminar, correr, saltar, caer), traducida al
   personaje de cada uno, con la velocidad de la animación según la de avance (como act_walking)
   y un cuadro inicial distinto por impostor para que no vayan todos al mismo paso. */
static struct MarioAnimation sImpAnimTab[IMP_MAX];

static void imp_set_anim_at(int i, s32 charAnim, s32 accel, s32 frame);
static void imp_set_anim(int i, s32 charAnim, s32 accel) { imp_set_anim_at(i, charAnim, accel, -1); }
static void imp_set_anim_ex(int i, s32 charAnim, s32 accel, bool fromStart) { imp_set_anim_at(i, charAnim, accel, fromStart ? -2 : -1); }
/* frame: -1 un cuadro al azar (los pasos de un grupo no van sincronizados), -2 desde el primero,
   >= 0 ese cuadro (el que manda el jugador) */
static void imp_set_anim_at(int i, s32 charAnim, s32 accel, s32 frame) {
    struct Object* o = sImp[i].o;
    s32 animId = get_character_anim(&sImpMS[i], (enum CharacterAnimID)charAnim);
    struct MarioAnimation* ma = &sImpAnimTab[i];
    load_patchable_table(ma, (u32)animId, true);
    struct Animation* t = ma->targetAnim;
    if (t == NULL) return;
    struct AnimInfo* an = &o->header.gfx.animInfo;
    if (an->animID != animId || an->curAnim != t) {
        an->animID = (s16)animId;
        an->curAnim = t;
        an->animYTrans = 0xBD;                         /* m->unkB0 de Mario */
        s32 len = t->loopEnd - t->startFrame;
        s32 start = t->startFrame + (frame == -1 && len > 1 ? (i * 7) % len : 0);
        if (frame >= 0) { start = frame; if (start < t->startFrame) start = t->startFrame; if (t->loopEnd > t->startFrame && start >= t->loopEnd) start = t->loopEnd - 1; }
        an->animFrameAccelAssist = start << 0x10;
        an->animFrame = (s16)start;
    }
    an->animAccel = accel;
}

static void imp_copy_lighting(int i) {
    struct MarioBodyState* d = &gBodyStates[MAX_PLAYERS + i];
    const struct MarioBodyState* l = gMarioStates[0].marioBodyState;
    if (l == NULL) {
        d->shadeR = d->shadeG = d->shadeB = 127;
        d->lightR = d->lightG = d->lightB = 255;
        d->lightingDirX = d->lightingDirY = d->lightingDirZ = 0;
        return;
    }
    /* la misma iluminación que el Mario propio (los mods de día y noche la cambian) */
    d->shadeR = l->shadeR; d->shadeG = l->shadeG; d->shadeB = l->shadeB;
    d->lightR = l->lightR; d->lightG = l->lightG; d->lightB = l->lightB;
    d->lightingDirX = l->lightingDirX; d->lightingDirY = l->lightingDirY; d->lightingDirZ = l->lightingDirZ;
}

/* formato 2: la animación que tiene el jugador, desde su cuadro (adelantado por el tiempo desde
   que llegó el estado); la velocidad de los pasos sale de la velocidad de avance, como en SM64 */
static void imp_animate_exact(int i, uint16_t id, int mult) {
    s32 anim = sEnt[id].pose;
    float fwd = sEnt[id].fwd;
    s32 accel = 0x10000;
    bool steps = anim == CHAR_ANIM_WALKING || anim == CHAR_ANIM_RUNNING || anim == CHAR_ANIM_TIPTOE ||
                 anim == CHAR_ANIM_CRAWLING || anim == CHAR_ANIM_WALK_WITH_LIGHT_OBJ || anim == CHAR_ANIM_RUN_WITH_LIGHT_OBJ ||
                 anim == CHAR_ANIM_SLOW_WALK_WITH_LIGHT_OBJ || anim == CHAR_ANIM_WALK_WITH_HEAVY_OBJ;
    if (steps) {
        float sp = fwd / 4.f;                          /* act_walking: forwardVel / 4 × 0x10000 */
        if (sp < 0.25f) sp = 0.25f;
        if (sp > 4.f) sp = 4.f;
        accel = (s32)(sp * 0x10000);
    }
    double el = smnet_now_ms() - sEnt[id].seen;
    if (el < 0) el = 0;
    if (el > 500) el = 500;
    s32 frame = (s32)sEnt[id].frame + (steps ? 0 : (s32)(el / (1000.0 / 30.0)));
    imp_set_anim_at(i, anim, accel * mult, frame);
    /* misma animación pero otro cuadro (otra patada, otro golpe): se vuelve a sincronizar; las que
       se repiten en bucle (pasos, quieto) no, para no saltar al dar la vuelta */
    if (!steps && sImp[i].o) {
        struct AnimInfo* an = &sImp[i].o->header.gfx.animInfo;
        struct Animation* t = an->curAnim;
        if (t && frame < t->loopEnd && (an->animFrame - frame > 10 || frame - an->animFrame > 10)) {
            an->animFrame = (s16)frame; an->animFrameAccelAssist = frame << 0x10;
        }
    }
    u32 act = (sEnt[id].sflags & STF_AIRE) ? ACT_JUMP : fwd >= 1.f ? ACT_WALKING : ACT_IDLE;
    sImpMS[i].action = act;
    sImpMS[i].forwardVel = fwd;
    gBodyStates[MAX_PLAYERS + i].action = act;
}

static void imp_animate(int i, float fwd, float vy, uint8_t pose, int mult) {
    s32 anim, accel = 0x10000;
    u32 act;
    bool air = (pose & POSE_AIRE) != 0, jump = true;
    if (air) {
        act = ACT_JUMP;
        switch (pose & 0x7F) {
            case POSE_SALTO:        anim = CHAR_ANIM_SINGLE_JUMP; break;
            case POSE_DOBLE:        anim = vy > 0.f ? CHAR_ANIM_DOUBLE_JUMP_RISE : CHAR_ANIM_DOUBLE_JUMP_FALL; act = ACT_DOUBLE_JUMP; break;
            case POSE_TRIPLE:       anim = CHAR_ANIM_TRIPLE_JUMP; act = ACT_TRIPLE_JUMP; break;
            case POSE_MORTAL_ATRAS: anim = CHAR_ANIM_BACKFLIP; act = ACT_BACKFLIP; break;
            case POSE_MORTAL_LADO:  anim = CHAR_ANIM_SLIDEFLIP; act = ACT_SIDE_FLIP; break;
            case POSE_SALTO_LARGO:  anim = fwd > 16.f ? CHAR_ANIM_FAST_LONGJUMP : CHAR_ANIM_SLOW_LONGJUMP; act = ACT_LONG_JUMP; break;
            case POSE_PATADA_PARED: anim = CHAR_ANIM_SLIDEJUMP; act = ACT_WALL_KICK_AIR; break;
            case POSE_GOLPE_SUELO:  anim = CHAR_ANIM_GROUND_POUND; act = ACT_GROUND_POUND; break;
            case POSE_CLAVADO:      anim = CHAR_ANIM_DIVE; act = ACT_DIVE; break;
            case POSE_GIRO:         anim = CHAR_ANIM_TWIRL; act = ACT_TWIRLING; jump = false; break;
            case POSE_CAIDA:        anim = CHAR_ANIM_GENERAL_FALL; act = ACT_FREEFALL; jump = false; break;
            /* en el aire por otra cosa (rebote, empujón...): subiendo, salto; bajando, caída */
            default:                if (vy > 0.f) { anim = CHAR_ANIM_SINGLE_JUMP; } else { anim = CHAR_ANIM_GENERAL_FALL; act = ACT_FREEFALL; jump = false; } break;
        }
        imp_set_anim_ex(i, anim, accel * mult, jump);
    } else {
        /* sin la pose (bots, versiones viejas): por la velocidad, como antes */
        if (pose == POSE_OTRO && vy > 4.f)       { anim = CHAR_ANIM_SINGLE_JUMP;   act = ACT_JUMP; }
        else if (pose == POSE_OTRO && vy < -10.f){ anim = CHAR_ANIM_GENERAL_FALL;  act = ACT_FREEFALL; }
        else if (fwd < 1.f) { anim = CHAR_ANIM_IDLE_HEAD_LEFT; act = ACT_IDLE; }
        else {
            anim = fwd < 12.f ? CHAR_ANIM_WALKING : CHAR_ANIM_RUNNING;
            act = ACT_WALKING;
            float sp = fwd / 4.f;                          /* act_walking: forwardVel / 4 × 0x10000 */
            if (sp < 0.25f) sp = 0.25f;
            if (sp > 4.f) sp = 4.f;
            accel = (s32)(sp * 0x10000);
        }
        imp_set_anim(i, anim, accel * mult);
    }
    sImpMS[i].action = act;
    sImpMS[i].forwardVel = fwd;
    gBodyStates[MAX_PLAYERS + i].action = act;
}

static void imp_animate_ent(int i, uint16_t id, int mult) {
    if ((sEnt[id].sflags & STF_V2) && sEnt[id].pose != ANIM_NONE) imp_animate_exact(i, id, mult);
    else imp_animate(i, sEnt[id].fwd, sEnt[id].vy, (sEnt[id].sflags & STF_V2) ? (uint8_t)((sEnt[id].sflags & STF_AIRE) ? POSE_AIRE : 0) : sEnt[id].pose, mult);
}

s32 ns_smnet_impostor_index(struct Object* o) {
    if (o == NULL || !sMassive || (o->oBehParams & 0xFFFF0000) != (s32)IMP_MARK) return -1;
    s32 i = o->oBehParams & 0xFFFF;
    if (i < 0 || i >= IMP_MAX || sImp[i].o != o || !sImp[i].mario) return -1;
    return i;
}
const struct PlayerPalette* ns_smnet_impostor_palette(s32 k) {
    if (k < 0 || k >= IMP_MAX) return &configPlayerPalette;
    return &sImp[k].pal;
}
u8 ns_smnet_impostor_model(s32 k) { return (k < 0 || k >= IMP_MAX) ? 0 : sImp[k].model; }
struct MarioState* ns_smnet_impostor_mario_state(s32 k) {
    if (k < 0 || k >= IMP_MAX) k = 0;
    if (sImpMS[k].marioBodyState == NULL) { sImpMS[k].marioBodyState = &gBodyStates[MAX_PLAYERS + k]; sImpMS[k].character = &gCharacters[0]; }
    return &sImpMS[k];
}

/* prepara las tablas de dibujo de Mario para el impostor i (estado de cuerpo, cámara, MarioState) */
static void imp_init_mario(int i, u8 model) {
    /* estado de cuerpo en cero (ojos que parpadean, puños, gorra puesta), la cabeza derecha */
    memset(&gBodyStates[MAX_PLAYERS + i], 0, sizeof(struct MarioBodyState));
    /* el color de cada parte es paleta × luz/sombra del estado de cuerpo: en cero, sale negro */
    imp_copy_lighting(i);
    memset(&gPlayerCameraState[MAX_PLAYERS + i], 0, sizeof(struct PlayerCameraState));
    memset(&sImpMS[i], 0, sizeof(struct MarioState));
    sImpMS[i].marioBodyState = &gBodyStates[MAX_PLAYERS + i];
    sImpMS[i].character = &gCharacters[model];
    sImpMS[i].playerIndex = 0;   /* los mods que lo usen como índice no deben salirse de los 16 */
    memset(&sImpAnimTab[i], 0, sizeof sImpAnimTab[i]);
    sImp[i].o->header.gfx.animInfo.animID = -1;
}

/* el Mario de pocas caras se carga una vez, en el pool permanente de modelos */
static s32 imp_low_model(void) {
    if (!sLowId) dynos_model_load_geo(&sLowId, MODEL_POOL_PERMANENT, (void*)ns_smnet_lowpoly_geo, true);
    return (s32)sLowId;
}
/* qué modelo le toca a cada nivel: 0 = completo, 1 = pocas caras, 2 = gorra. Los bajos y los
   lejanos van con el de pocas caras (mismo esqueleto: se animan igual, 132 triángulos y sin las
   funciones de la cabeza, los ojos y las manos del completo) */
static int imp_kind_for(u8 tier) {
    if (tier == IMP_MEDIO) return 0;
    if (tier == IMP_BAJO) return sImpBajoCompleto ? 0 : 1;
    if (sImpLejCompleto) return 0;
    return sImpGorra ? 2 : 1;
}
static s32 imp_kind_model(int kind, u8 model, uint16_t ext) {
    return kind == 0 ? imp_model_for_ext(ext, model) : kind == 1 ? imp_low_model() : MODEL_MARIOS_CAP;
}

/* aplica el nivel: modelo (en el sitio), sombra, ojos y pose */
static void imp_apply_tier(int i, u8 tier) {
    struct Object* o = sImp[i].o;
    int kind = imp_kind_for(tier);
    bool wantMario = kind != 2, wantLow = kind == 1;
    if (wantMario != sImp[i].mario || wantLow != sImp[i].low) {
        obj_set_model(o, imp_kind_model(kind, sImp[i].model, sImp[i].ext));
        obj_scale(o, wantMario ? 1.0f : IMP_SCALE);
        bool wasMario = sImp[i].mario;
        sImp[i].mario = wantMario;
        sImp[i].low = wantLow;
        /* completo ↔ pocas caras: la animación se vuelve a elegir enseguida (sin un cuadro sin pose) */
        if (wantMario && !wasMario) imp_init_mario(i, sImp[i].model);
        else o->header.gfx.animInfo.curAnim = NULL;
        if (wantMario && sImp[i].id < SMNET_MAX_PLAYERS) imp_animate_ent(i, sImp[i].id, 1);
    }
    sImp[i].tier = tier;
    o->header.gfx.shadowInvisible = (tier != IMP_MEDIO);   /* ver geo_process_shadow (SMNET) */
    if (sImp[i].mario) {
        /* ojos: 0 = parpadeo propio; abiertos fijos en los de menos detalle */
        gBodyStates[MAX_PLAYERS + i].eyeState = tier == IMP_MEDIO ? 0 : MARIO_EYES_OPEN;
        imp_copy_lighting(i);
        if (tier == IMP_LEJANO && sImpLejQuietos) {
            /* pose quieta: la de estar parado, sin avanzar (aceleración mínima) */
            imp_set_anim(i, CHAR_ANIM_IDLE_HEAD_LEFT, 1);
            sImpMS[i].action = ACT_IDLE; sImpMS[i].forwardVel = 0;
            gBodyStates[MAX_PLAYERS + i].action = ACT_IDLE;
        }
    }
}

/* niveles de "Multitud" (Opciones > Pantalla): medios, bajos y el total; los lejanos son el
   resto hasta el total, y en Alta y Muy alta no hay tope (todos los que estén en el área) */
#define CROWD_SIN_TOPE (-1)
static const int sCrowd[5][3] = { { 16, 32, 384 }, { 32, 64, 512 }, { 64, 96, 768 }, { 96, 128, CROWD_SIN_TOPE }, { 128, 192, CROWD_SIN_TOPE } };
static int sImpEnvMed = -1, sImpEnvBajo = -1, sImpEnvMax = -1;   /* las variables de entorno mandan sobre el menú */
/* Ajustes → Partida masiva (configfile.c); las variables COOPDX_SMNET_* de antes siguen mandando
   si están puestas. Se aplica en el momento (el panel la llama al cambiar algo). */
void ns_smnet_options_apply(void) {
    sImpGorra = configSmnetFarMode == 1;
    sImpLejCompleto = configSmnetFarMode == 2;
    sImpBajoCompleto = configSmnetLowFull;
    sImpLejQuietos = !configSmnetFarAnimated;
    const char* e;
    if ((e = getenv("COOPDX_SMNET_LEJANOS_GORRA")) && e[0]) sImpGorra = e[0] == '1';
    if ((e = getenv("COOPDX_SMNET_LEJANOS_COMPLETOS")) && e[0]) sImpLejCompleto = e[0] == '1';
    if ((e = getenv("COOPDX_SMNET_BAJOS_COMPLETOS")) && e[0]) sImpBajoCompleto = e[0] == '1';
    if ((e = getenv("COOPDX_SMNET_LEJANOS_QUIETOS")) && e[0]) sImpLejQuietos = e[0] == '1';
}

static void imp_read_config(void) {
    extern unsigned int configSmnetCrowd;
    unsigned q = configSmnetCrowd > 4 ? 2 : configSmnetCrowd;
    sImpMario = sImpEnvMed >= 0 ? sImpEnvMed : sCrowd[q][0];
    sImpBajo  = sImpEnvBajo >= 0 ? sImpEnvBajo : sCrowd[q][1];
    sImpMax   = sImpEnvMax >= 0 ? sImpEnvMax : (sCrowd[q][2] == CROWD_SIN_TOPE ? IMP_MAX : sCrowd[q][2]);
    if (sImpMax > IMP_MAX) sImpMax = IMP_MAX;
}

static int imp_tier_lo(int t) { return t == IMP_MEDIO ? -1000000 : t == IMP_BAJO ? sImpMario : sImpMario + sImpBajo; }
static int imp_tier_hi(int t) { return t == IMP_MEDIO ? sImpMario : t == IMP_BAJO ? sImpMario + sImpBajo : 1000000; }
static u8 imp_tier_for(int k) { return k < sImpMario ? IMP_MEDIO : k < sImpMario + sImpBajo ? IMP_BAJO : IMP_LEJANO; }

/* los visibles sin slot se dibujan como un objeto liviano (sin física ni colisión) que sigue los
   12 B de estado de smnet. Los objetos los borra el juego al cambiar de área: se reconoce el
   cambio y se vuelven a crear. */
#define IMP_HYST 10
#define IMP_PER_TICK 64
struct ImpCand { float d; uint16_t id; };
static int imp_cand_cmp(const void* a, const void* b) {
    float x = ((const struct ImpCand*)a)->d, y = ((const struct ImpCand*)b)->d;
    return x < y ? -1 : x > y ? 1 : (int)((const struct ImpCand*)a)->id - (int)((const struct ImpCand*)b)->id;
}
static void impostors_update(double now) {
    extern s16 gCurrActStarNum, gCurrLevelNum, gCurrAreaIndex;
    struct MarioState* m = &gMarioStates[0];
    if (m->marioObj == NULL || gNetworkPlayerLocal == NULL || !gNetworkPlayerLocal->currAreaSyncValid) { imp_forget_all(false); return; }
    if (gCurrLevelNum != sImpLevel || gCurrAreaIndex != sImpArea || gCurrActStarNum != sImpAct) {
        imp_forget_all(false);        /* el juego ya descargó los objetos del área anterior */
        sImpLevel = gCurrLevelNum; sImpArea = gCurrAreaIndex; sImpAct = gCurrActStarNum;
        sLastImp = 0;
    }
    for (int i = 0; i < IMP_MAX; i++) if (sImp[i].o && !imp_alive(i)) imp_drop(i, false);

    if (now - sLastImp >= SLOT_EVERY_MS) {
        sLastImp = now;
        imp_read_config();
        smnet_node_info_t inf;
        smnet_node_info(sNode, &inf);
        float ox = m->pos[0], oy = m->pos[1], oz = m->pos[2];
        static uint16_t cand[SMNET_MAX_PLAYERS];
        static int16_t rank[SMNET_MAX_PLAYERS];
        int nc = 0;
        static struct ImpCand cs[SMNET_MAX_PLAYERS];
        for (int id = 0; id < SMNET_MAX_PLAYERS; id++) {
            rank[id] = -1;
            if (id == inf.id || (sSlotOf[id] && sSlotReady[sSlotOf[id]]) || sEnt[id].seen <= 0 || now - sEnt[id].seen > IMP_SEEN_MS) continue;
            float dx = sEnt[id].x - ox, dy = sEnt[id].y - oy, dz = sEnt[id].z - oz;
            cs[nc].d = dx * dx + dy * dy + dz * dz;
            cs[nc].id = (uint16_t)id;
            nc++;
        }
        qsort(cs, (size_t)nc, sizeof cs[0], imp_cand_cmp);   /* hasta 1023: la inserción era O(n²) */
        for (int k = 0; k < nc; k++) cand[k] = cs[k].id;
        for (int k = 0; k < nc; k++) rank[cand[k]] = (int16_t)k;
        /* histéresis: se crea hasta el puesto sImpMax, pero uno que ya existe se mantiene hasta
           IMP_HYST puestos más allá; lo mismo en los bordes de cada nivel. Cambiar de nivel es
           barato (no se recrea el objeto). */
        for (int i = 0; i < IMP_MAX; i++) {
            if (sImp[i].id == SMNET_NO_ID) continue;
            int k = rank[sImp[i].id];
            if (k < 0 || k >= sImpMax + IMP_HYST) { imp_drop(i, true); continue; }
            int t = sImp[i].tier;
            if (k < imp_tier_lo(t) - IMP_HYST || k >= imp_tier_hi(t) + IMP_HYST) {
                imp_apply_tier(i, imp_tier_for(k));
                sImpTierChg++;
            }
        }
        /* altas */
        int ncNew = nc < sImpMax ? nc : sImpMax;
        int freeHint = 0, made = 0;
        for (int k = 0; k < ncNew; k++) {
            uint16_t id = cand[k];
            if (sImpOf[id] >= 0) continue;
            /* al entrar a un área con cientos: de a IMP_PER_TICK por cuadro (sin un tirón) */
            if (made >= IMP_PER_TICK) { sLastImp = now - SLOT_EVERY_MS; break; }
            int i = freeHint;
            while (i < IMP_MAX && sImp[i].id != SMNET_NO_ID) i++;
            if (i == IMP_MAX) break;
            freeHint = i + 1;
            made++;
            /* que siempre quede lugar en el pool de objetos para el nivel (enemigos, monedas…) */
            { extern s16 gPrevFrameObjectCount; if (gPrevFrameObjectCount > OBJECT_POOL_CAPACITY - 200) break; }
            u8 tier = imp_tier_for(k);
            int kind = imp_kind_for(tier);
            bool asMario = kind != 2;
            u8 model = 0;
            struct PlayerPalette pal;
            memcpy(&pal, &configPlayerPalette, sizeof pal);
            if (sInfo[id].len >= 1 + (int)sizeof pal) { model = sInfo[id].d[0]; memcpy(&pal, sInfo[id].d + 1, sizeof pal); }
            if (model >= CT_MAX) model = 0;
            uint16_t ext = info_model_ext(id);
            struct Object* o = spawn_object(m->marioObj, imp_kind_model(kind, model, ext), smnet_static_bhv());
            if (o == NULL) break;              /* sin lugar en el pool de objetos */
            sImp[i].o = o;
            sImp[i].mario = asMario;
            sImp[i].low = kind == 1;
            sImp[i].ext = ext;
            sImp[i].model = model;
            sImp[i].pal = pal;
            if (asMario) imp_init_mario(i, model);
            o->oBehParams = (s32)(IMP_MARK | (u32)i);
            { float fl = find_floor_height(sEnt[id].x, sEnt[id].y + 200.f, sEnt[id].z);
              sImp[i].fy = fl > -10000.f ? fl : sEnt[id].y; sImp[i].fx = sEnt[id].x; sImp[i].fz = sEnt[id].z; sImp[i].fage = 0;
              sImp[i].gdy = ((sEnt[id].sflags & STF_V2) ? (sEnt[id].sflags & STF_ALTURA) : (sEnt[id].pose & POSE_AIRE)) ? 0.f : sImp[i].fy - sEnt[id].y; }
            o->oPosX = sEnt[id].x; o->oPosY = sImp[i].fy + (asMario ? 0.f : IMP_HEAD_Y); o->oPosZ = sEnt[id].z;
            o->oFaceAngleYaw = o->oMoveAngleYaw = sEnt[id].yaw;
            o->oFaceAnglePitch = o->oFaceAngleRoll = 0;
            obj_scale(o, asMario ? 1.0f : IMP_SCALE);
            o->oOpacity = 255;                 /* el geo de la gorra usa la opacidad del objeto */
            o->globalPlayerIndex = 0;
            sImp[i].id = id;
            sImpOf[id] = (int16_t)i;
            imp_apply_tier(i, tier);
            sImpShown++;
        }
    }
    /* cada cuadro: acercarse a la última posición conocida; lo caro (piso, animación, luz) se
       hace con menos frecuencia en los niveles bajos, repartido entre cuadros según el índice */
    sImpFrame++;
    double nowF = smnet_now_ms();
    for (int i = 0; i < IMP_MAX; i++) {
        if (!sImp[i].o) continue;
        struct Object* o = sImp[i].o;
        uint16_t id = sImp[i].id;
        u8 tier = sImp[i].tier;
        unsigned ph = sImpFrame + (unsigned)i;
        /* predicción: entre un dato y el siguiente (los lejanos llegan a 10 o 2 Hz) se sigue
           avanzando con su dirección y velocidad (u por cuadro, como forwardVel de SM64),
           hasta 0,6 s; así no se queda quieta y salta cuando llega el dato nuevo */
        float tx = sEnt[id].x, tz = sEnt[id].z;
        float fwd = sEnt[id].fwd;
        if (fwd > 0.5f && fwd < 200.f) {
            double el = nowF - sEnt[id].seen;
            if (el > 600) el = 600;
            if (el > 0) {
                float a = (float)sEnt[id].yaw * (3.14159265f / 32768.f);
                float adv = fwd * (float)(el / (1000.0 / 30.0));
                tx += sinf(a) * adv;
                tz += cosf(a) * adv;
            }
        }
        /* apoyado en el piso de abajo: la altura que manda cada uno puede no coincidir con el
           terreno de este mundo (los bots mandan siempre la de la plantilla). El piso solo se
           vuelve a buscar si se movió (un grupo parado no cuesta nada) o cada tanto */
        {
            float mx = tx - sImp[i].fx, mz = tz - sImp[i].fz, mv = mx * mx + mz * mz;
            float step = tier == IMP_MEDIO ? 4.f : tier == IMP_BAJO ? 24.f : 60.f;
            unsigned every = tier == IMP_MEDIO ? 1 : tier == IMP_BAJO ? 3 : 10;
            sImp[i].fage++;
            if ((mv > step * step && ph % every == 0) || sImp[i].fage > 90) {
                float fl = find_floor_height(tx, sEnt[id].y + 200.f, tz);
                sImp[i].fy = fl > -10000.f ? fl : sEnt[id].y;
                sImp[i].fx = tx; sImp[i].fz = tz; sImp[i].fage = 0;
            }
        }
        float ty = sImp[i].fy + (sImp[i].mario ? 0.f : IMP_HEAD_Y);
        /* en el aire (salto, rebote, caída): la altura que manda, adelantada con su velocidad
           vertical (gravedad de SM64: -4 por cuadro) hasta que llegue el próximo estado; nunca
           por debajo del piso. En el suelo sigue apoyado en el piso de este mundo. */
        bool v2 = (sEnt[id].sflags & STF_V2) != 0;
        bool inAir = v2 ? (sEnt[id].sflags & STF_ALTURA) != 0 : (sEnt[id].pose & POSE_AIRE) != 0;
        bool grav = v2 ? (sEnt[id].sflags & STF_AIRE) != 0 : inAir;
        /* diferencia entre la altura que manda en el suelo y el piso de este mundo (los bots mandan
           la de la plantilla; un jugador real, casi cero): el salto se dibuja desde el piso */
        if (!inAir) {
            float g = sImp[i].fy - sEnt[id].y;
            sImp[i].gdy = (g > 3000.f || g < -3000.f) ? 0.f : g;
        }
        if (inAir) {
            double el = nowF - sEnt[id].seen;
            if (el < 0) el = 0;
            if (el > 200) el = 200;
            float fr = (float)(el / (1000.0 / 30.0));
            float ey = sEnt[id].y + sImp[i].gdy + (grav ? sEnt[id].vy * fr - 2.f * fr * fr : 0.f);
            if (ey < sImp[i].fy) ey = sImp[i].fy;
            ty = ey + (sImp[i].mario ? 0.f : IMP_HEAD_Y);
        }
        if (sImp[i].mario) {
            if (tier == IMP_MEDIO) { imp_copy_lighting(i); imp_animate_ent(i, id, 1); }
            else {
                /* bajos: 1 de cada 3 cuadros avanza 3 pasos (10 Hz); lejanos: 1 de cada 6, 6
                   pasos (5 Hz); en los otros queda quieta */
                unsigned st = tier == IMP_BAJO ? 3 : 6;
                if (tier == IMP_LEJANO && sImpLejQuietos) { if (ph % 30 == 0) imp_copy_lighting(i); }
                else if (ph % st == 0) { if (tier == IMP_BAJO || ph % 30 < st) imp_copy_lighting(i); imp_animate_ent(i, id, (int)st); }
                else o->header.gfx.animInfo.animAccel = 1;
            }
        }
        float dx = tx - o->oPosX, dy = ty - o->oPosY, dz = tz - o->oPosZ;
        if (dx * dx + dy * dy + dz * dz > 1500.f * 1500.f) { o->oPosX = tx; o->oPosY = ty; o->oPosZ = tz; }
        else { o->oPosX += dx * 0.35f; o->oPosY += dy * (inAir ? 0.6f : 0.35f); o->oPosZ += dz * 0.35f; }
        o->oFaceAngleYaw = o->oMoveAngleYaw = sEnt[id].yaw;
    }
}

static void on_entity(void* user, uint16_t id, uint8_t seq, const smnet_state_t* st, double now) {
    (void)user; (void)seq;
    if (id >= SMNET_MAX_PLAYERS || ns_smnet_mod_hidden(id)) return;   /* echado o baneado: no se dibuja */
    sEnt[id].x = st->x; sEnt[id].y = st->y; sEnt[id].z = st->z; sEnt[id].yaw = st->yaw; sEnt[id].fwd = st->fwd_vel; sEnt[id].vy = st->vel_y; sEnt[id].pose = st->action; sEnt[id].sflags = st->flags; sEnt[id].frame = st->anim_frame;
    sEnt[id].seen = now;
}

static void on_player(void* user, uint16_t id, int joined, const uint8_t* info, int len) {
    (void)user;
    if (id >= SMNET_MAX_PLAYERS) return;
    { smnet_node_info_t inf; smnet_node_info(sNode, &inf); if (id == inf.id) return; }   /* uno mismo no va en la lista */
    /* el nodo de un Join que todavía baja los mods (ns_smnet_prejoin.c): no es un jugador */
    if (joined && len >= 1 && info[0] == NS_SMNET_PREJOIN_MARK) joined = 0;
    if (joined && ns_smnet_mod_hidden(id)) joined = 0;   /* echado o baneado */
    if (!joined && id == sHostId) { sHostId = SMNET_NO_ID; smlog("el host salió de la partida"); }
    if (joined && !sKnown[id] && sOrigHost) host_claim_send(SMNET_TO_PLAYER, id);
    if (joined && !sKnown[id]) { sKnown[id] = 1; sKnownCount++; lua_player_event(id, 1); }
    if (!joined && sKnown[id]) { sKnown[id] = 0; sKnownCount--; lua_player_event(id, 0); }
    if (!joined) {
        sInfo[id].len = 0;
        if (sSlotOf[id]) slot_free(sSlotOf[id]);
        sEnt[id].seen = 0;
        return;
    }
    if (len > SMNET_MAX_PINFO) len = SMNET_MAX_PINFO;
    if (len < 0) len = 0;
    sInfo[id].len = (uint8_t)len;
    memcpy(sInfo[id].d, info, (size_t)len);
    if (sSlotOf[id]) slot_apply_info(sSlotOf[id], id);
    /* un impostor creado antes de que llegaran sus datos tenía la paleta propia: se corrige; si
       cambió de personaje se recrea con el modelo nuevo */
    if (sImpOf[id] >= 0 && len >= 1 + (int)sizeof(struct PlayerPalette)) {
        int i = sImpOf[id];
        memcpy(&sImp[i].pal, info + 1, sizeof(struct PlayerPalette));
        u8 model = info[0] < CT_MAX ? info[0] : 0;
        uint16_t ext = info_model_ext(id);
        if (model != sImp[i].model || ext != sImp[i].ext) {
            sImp[i].model = model;
            sImp[i].ext = ext;
            if (sImp[i].mario && !sImp[i].low && imp_alive(i)) {   /* el modelo se cambia en el sitio */
                obj_set_model(sImp[i].o, imp_model_for_ext(ext, model));
                imp_init_mario(i, model);
                imp_apply_tier(i, sImp[i].tier);
            }
        }
    }
}

/* el PLAYER de un jugador con slot: se descomprime, se le pone el slot como índice global y se
   procesa como si hubiera llegado de ese "cliente" */
static void massive_inject(uint16_t id, const uint8_t* blob, int len) {
    if (id >= SMNET_MAX_PLAYERS) return;
    int s = sSlotOf[id];
    if (s <= 0) { sPl.desconocido++; return; }
    struct Packet* p = &sRxPacket;
    memset(p, 0, sizeof *p);
    p->localIndex = (u8)s;
    p->cursor = 3;
    sBlobFrom = id;
    p->addr = &sBlobFrom;
    if (!packet_decompress(p, (u8*)blob, (u32)len) || p->dataLength < 7 || p->buffer[0] != PACKET_PLAYER) { sPl.malo++; return; }
    uint8_t flags = p->buffer[3];
    int off = 5;
    if (flags & 4) off += 5;
    if (!(flags & 1) || off + 5 >= (int)p->dataLength) { sPl.malo++; return; }
    struct NetworkPlayer* np = &gNetworkPlayers[s];
    s16 level;
    memcpy(&level, &p->buffer[off + 2], sizeof level);
    np->currCourseNum = p->buffer[off];
    np->currActNum = p->buffer[off + 1];
    np->currLevelNum = level;
    np->currAreaIndex = p->buffer[off + 4];
    /* nivel nunca "sincronizado": al entrar a un área, sm64coopdx le pide el estado a alguien
       con nivel y área sincronizados ahí (get_network_player_from_area / _from_level); ese
       pedido a un slot no sale en este modo, el área propia no se validaba y sin área válida no
       se manda el PLAYER propio (solo se veía al primero que entró). El de área sí, para los
       nombres sobre la cabeza (nametags.c). Tampoco cuentan para el dueño de los objetos
       (get_network_player_smallest_global): cada uno es dueño de los de su mundo. */
    np->currLevelSyncValid = false;
    np->currAreaSyncValid = true;
    np->lastReceived = clock_elapsed();
    p->buffer[off + 5] = (u8)s;           /* índice global = slot */
    packet_receive(p);
    sPl.rx++;
    /* cambio limpio impostor → Mario completo: sin el fundido de entrada de sm64coopdx (el humo)
       y el impostor se va en el mismo cuadro en que el Mario completo tiene datos */
    np->fadeOpacity = 32;
    /* los bots mandan siempre la altura de la plantilla: en el terreno de este mundo quedaban bajo
       el piso (desaparecían cerca, en las subidas). A los que no simulan se los apoya en el piso. */
    if (!ns_smnet_slot_simulates((u8)s)) {
        struct MarioState* bm = &gMarioStates[s];
        f32 fl = find_floor_height(bm->pos[0], bm->pos[1] + 400.f, bm->pos[2]);
        if (fl > -10000.f) {
            bm->pos[1] = fl;
            if (bm->marioObj) { bm->marioObj->oPosY = fl; bm->marioObj->header.gfx.pos[1] = fl; }
        }
    }
    if (!sSlotReady[s]) {
        sSlotReady[s] = true;
        if (sImpOf[id] >= 0) imp_drop(sImpOf[id], true);
    }
}

/* COOPDX_SMNET_GRABAR=archivo: mientras Mario corre, cada segundo se guarda el PLAYER propio (sin comprimir) como
   plantilla para smnet-mario-bots, con la posición de los campos que los bots cambian.
   Formato: "SMPL" u16 versión(1) u16 inicio_datos u16 off_pos u16 off_angulo u16 off_yaw_deseado
            u16 tam_datos u16 largo, y los bytes del paquete (sin el hash). */
static void maybe_record_template(const struct Packet* p) {
    static double last = 0;
    const char* path = getenv("COOPDX_SMNET_GRABAR");
    if (!path || !path[0]) return;
    double now = smnet_now_ms();
    if (now - last < 1000) return;
    /* solo mientras Mario corre: los bots repiten la acción y la animación de la plantilla y
       así se los ve caminar (con una plantilla quieta se deslizan parados) */
    if (gMarioStates[0].forwardVel < 10.f) return;
    last = now;
    uint8_t flags = p->buffer[3];
    int off = 5 + ((flags & 4) ? 5 : 0) + ((flags & 1) ? 5 : ((flags & 8) ? 4 : 0));
    u16 hdr[7];
    u16 opos, oface, oyaw, osize;
    ns_smnet_player_offsets(&opos, &oface, &oyaw, &osize);
    hdr[0] = 1; hdr[1] = (u16)(off + 1); hdr[2] = opos; hdr[3] = oface; hdr[4] = oyaw; hdr[5] = osize; hdr[6] = p->dataLength;
    if (off + 1 + osize > p->dataLength) return;
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite("SMPL", 1, 4, f);
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(p->buffer, 1, p->dataLength, f);
    fclose(f);
    smlog("plantilla de PLAYER grabada en %s (%u B)", path, p->dataLength);
}

void ns_smnet_publish_player_packet(struct Packet* p) {
    if (!sNode || p == NULL || p->error) return;
    maybe_record_template(p);
    packet_set_flags(p);
    u32 hash = packet_hash(p);
    memcpy(&p->buffer[p->dataLength], &hash, sizeof(u32));
    u8* buf = NULL;
    u32 len = 0;
    packet_compress(p, &buf, &len);
    if (!buf || len == 0) return;
    extern s16 gCurrActStarNum, gCurrLevelNum, gCurrAreaIndex;
    publish_player(area_key((unsigned)gCurrLevelNum, (unsigned)gCurrAreaIndex, (unsigned)gCurrActStarNum), buf, (int)len);
}

/* ---- modo masivo: chat global (a todos por la malla de SN, límite de smnet: 4/s, ráfaga 16) */
static unsigned long long sChatTx, sChatRx, sChatDrop;

void ns_smnet_send_chat(const char* message) {
    if (!sNode || !message) return;
    if (sMassive && ns_smnet_mod_muted(ns_smnet_self_smnet_id())) { djui_chat_message_create("\\#ffd080\\Estás silenciado: los demás no ven tus mensajes."); return; }
    uint8_t b[1 + MAX_CHAT_MSG_LENGTH];
    int n = (int)strlen(message);
    if (n > MAX_CHAT_MSG_LENGTH - 1) n = MAX_CHAT_MSG_LENGTH - 1;
    b[0] = APP_CHAT;
    memcpy(b + 1, message, (size_t)n);
    int rc = smnet_node_send_msg(sNode, SMNET_TO_ALL, 0, b, n + 1);
    if (rc == 0) sChatTx++;
    else {
        sChatDrop++;
        djui_chat_message_create("\\#ffa0a0\\(mensaje no enviado: demasiados seguidos)");
    }
    smlog("chat enviado (%d B, rc=%d)", n, rc);
}

static void massive_chat_rx(uint16_t from, const uint8_t* d, int len) {
    if (len <= 0 || from >= SMNET_MAX_PLAYERS) return;
    char text[MAX_CHAT_MSG_LENGTH];
    int n = len < MAX_CHAT_MSG_LENGTH - 1 ? len : MAX_CHAT_MSG_LENGTH - 1;
    for (int i = 0; i < n; i++) {
        char c = (char)d[i];
        text[i] = (c == '\\' || c == '\n' || c == '\r' || (unsigned char)c < 0x20) ? ' ' : c;   /* sin códigos de color ajenos */
    }
    text[n] = '\0';
    if (found_match(text)) { sChatDrop++; return; }
    /* nombre y color (gorra) de los datos de presentación: [modelo][paleta 24 B][nombre] */
    char name[MAX_CONFIG_STRING];
    snprintf(name, sizeof name, "Jugador %u", (unsigned)from);
    unsigned r = 0xdc, g = 0xdc, bl = 0xdc;
    int il = sInfo[from].len;
    const uint8_t* info = sInfo[from].d;
    if (il >= 1 + (int)sizeof(struct PlayerPalette)) {
        const uint8_t* cap = info + 1 + 3 * CAP;
        r = cap[0]; g = cap[1]; bl = cap[2];
        if (r + g + bl < 150) { r += 80; g += 80; bl += 80; }   /* que se lea sobre el fondo oscuro */
        int nl = il - 1 - (int)sizeof(struct PlayerPalette);
        if (nl > 0) {
            if (nl > (int)sizeof name - 1) nl = (int)sizeof name - 1;
            for (int i = 0; i < nl; i++) { char c = (char)info[1 + sizeof(struct PlayerPalette) + i]; name[i] = (c == '\\' || (unsigned char)c < 0x20) ? ' ' : c; }
            name[nl] = '\0';
        }
    }
    char line[MAX_CHAT_MSG_LENGTH + MAX_CONFIG_STRING + 32];
    snprintf(line, sizeof line, "\\#%02x%02x%02x\\%s\\#dcdcdc\\: %s", r & 0xff, g & 0xff, bl & 0xff, name, text);
    djui_chat_message_create(line);
    LOG_INFO("[smnet:%u] %s: %s", (unsigned)from, name, text);
    sChatRx++;
}

/* ---------------------------------------------------------------- API de Lua (mods)
 *
 * En el modo masivo los índices de sm64coopdx (0..15) son slots que rotan: el mismo índice puede
 * ser otra persona un rato después. Estas funciones dan a los mods la identidad estable de cada
 * jugador (el id de smnet, 0..1023) y la lista completa de la partida, no solo los 15 cercanos.
 *
 *   smnet_active()              -> true si está el modo masivo
 *   smnet_local_id()            -> id propio (o nil mientras entra)
 *   smnet_player_count()        -> jugadores en la partida, contándose
 *   smnet_players()             -> tabla con los ids de todos los demás
 *   smnet_player_name(id)       -> nombre (o nil)
 *   smnet_player_slot(id)       -> índice local (0 = uno mismo, 1..15) o nil si no tiene slot
 *   smnet_slot_player(indice)   -> id de quien ocupa ese índice local (o nil)
 *   smnet_player_pos(id)        -> x, y, z si se lo ve en el área (o nil)
 *   smnet_send(id, texto)       -> mensaje de mod a un jugador (id >= 0) o a todos (id = -1);
 *                                  true si salió (máx. 1000 B; a todos: 4/s, ráfaga 16)
 *   smnet_on_message(fn)        -> fn(id_origen, texto) al llegar un mensaje de mod
 *   smnet_on_player(fn)         -> fn(id, entro) cuando alguien entra (true) o se va (false)
 */
#ifndef NS_SMNET_SIN_LUA
#define LUA_CB_MAX 16
static lua_State* sLua = NULL;
static int sLuaMsgRef[LUA_CB_MAX], sLuaMsgN = 0;
static int sLuaPlayerRef[LUA_CB_MAX], sLuaPlayerN = 0;

static int lua_id_arg(lua_State* L, int i) {
    if (!lua_isnumber(L, i)) return -1;
    lua_Integer v = lua_tointeger(L, i);
    return (v >= 0 && v < SMNET_MAX_PLAYERS) ? (int)v : -1;
}

static int l_active(lua_State* L) { lua_pushboolean(L, ns_smnet_massive()); return 1; }

static int l_local_id(lua_State* L) {
    if (!sNode) { lua_pushnil(L); return 1; }
    smnet_node_info_t inf;
    smnet_node_info(sNode, &inf);
    if (inf.id == SMNET_NO_ID) lua_pushnil(L); else lua_pushinteger(L, inf.id);
    return 1;
}

static int l_player_count(lua_State* L) { lua_pushinteger(L, sNode ? sKnownCount + 1 : 0); return 1; }

static int l_players(lua_State* L) {
    lua_newtable(L);
    int k = 1;
    for (int id = 0; id < SMNET_MAX_PLAYERS; id++) if (sKnown[id]) { lua_pushinteger(L, id); lua_rawseti(L, -2, k++); }
    return 1;
}

static int l_player_name(lua_State* L) {
    int id = lua_id_arg(L, 1);
    smnet_node_info_t inf;
    if (sNode) smnet_node_info(sNode, &inf); else inf.id = SMNET_NO_ID;
    if (id >= 0 && id == inf.id) { lua_pushstring(L, configPlayerName); return 1; }
    if (id < 0 || !sKnown[id]) { lua_pushnil(L); return 1; }
    int n = sInfo[id].len - 1 - (int)sizeof(struct PlayerPalette);
    if (n <= 0) { lua_pushfstring(L, "Jugador %d", id); return 1; }
    lua_pushlstring(L, (const char*)sInfo[id].d + 1 + sizeof(struct PlayerPalette), (size_t)n);
    return 1;
}

static int l_player_slot(lua_State* L) {
    int id = lua_id_arg(L, 1);
    smnet_node_info_t inf;
    if (sNode) smnet_node_info(sNode, &inf); else inf.id = SMNET_NO_ID;
    if (id >= 0 && id == inf.id) { lua_pushinteger(L, 0); return 1; }
    if (id < 0 || sSlotOf[id] <= 0) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, sSlotOf[id]);
    return 1;
}

static int l_slot_player(lua_State* L) {
    lua_Integer s = lua_isnumber(L, 1) ? lua_tointeger(L, 1) : -1;
    if (s == 0) return l_local_id(L);
    if (s < 1 || s >= MAX_PLAYERS || sIds[s] == SMNET_NO_ID || !sMassive) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, sIds[s]);
    return 1;
}

static int l_player_pos(lua_State* L) {
    int id = lua_id_arg(L, 1);
    if (id < 0 || sEnt[id].seen <= 0 || smnet_now_ms() - sEnt[id].seen > SLOT_SEEN_MS) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, sEnt[id].x); lua_pushnumber(L, sEnt[id].y); lua_pushnumber(L, sEnt[id].z);
    return 3;
}

static int l_send(lua_State* L) {
    if (!sNode || !sMassive || !lua_isnumber(L, 1) || !lua_isstring(L, 2)) { lua_pushboolean(L, 0); return 1; }
    lua_Integer to = lua_tointeger(L, 1);
    size_t n = 0;
    const char* txt = lua_tolstring(L, 2, &n);
    if (n > 1000 || (to != -1 && (to < 0 || to >= SMNET_MAX_PLAYERS))) { lua_pushboolean(L, 0); return 1; }
    uint8_t b[1001];
    b[0] = APP_MOD;
    memcpy(b + 1, txt, n);
    int rc = to == -1 ? smnet_node_send_msg(sNode, SMNET_TO_ALL, 0, b, (int)n + 1)
                      : smnet_node_send_msg(sNode, SMNET_TO_PLAYER, (uint16_t)to, b, (int)n + 1);
    lua_pushboolean(L, rc == 0);
    return 1;
}

static int l_on_message(lua_State* L) {
    if (!lua_isfunction(L, 1) || sLuaMsgN == LUA_CB_MAX) return 0;
    lua_pushvalue(L, 1);
    sLuaMsgRef[sLuaMsgN++] = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

static int l_on_player(lua_State* L) {
    if (!lua_isfunction(L, 1) || sLuaPlayerN == LUA_CB_MAX) return 0;
    lua_pushvalue(L, 1);
    sLuaPlayerRef[sLuaPlayerN++] = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

static void lua_player_event(uint16_t id, int joined) {
    lua_State* L = sLua;
    if (!L || L != gLuaState) return;
    for (int i = 0; i < sLuaPlayerN; i++) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, sLuaPlayerRef[i]);
        lua_pushinteger(L, id);
        lua_pushboolean(L, joined);
        if (smlua_pcall(L, 2, 0, 0) != 0) { LOG_ERROR("smnet_on_player: error en el mod"); }
    }
}

static void lua_mod_message(uint16_t from, const uint8_t* d, int len) {
    lua_State* L = sLua;
    if (!L || L != gLuaState || len < 0) return;
    for (int i = 0; i < sLuaMsgN; i++) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, sLuaMsgRef[i]);
        lua_pushinteger(L, from);
        lua_pushlstring(L, (const char*)d, (size_t)len);
        if (smlua_pcall(L, 2, 0, 0) != 0) { LOG_ERROR("smnet_on_message: error en el mod"); }
    }
}

void ns_smnet_lua_bind(struct lua_State* L) {
    /* se llama en cada arranque de Lua (al hostear o cambiar de mods): las funciones viejas
       registradas murieron con el estado anterior */
    sLua = L;
    sLuaMsgN = sLuaPlayerN = 0;
    smlua_bind_function(L, "smnet_active", l_active);
    smlua_bind_function(L, "smnet_local_id", l_local_id);
    smlua_bind_function(L, "smnet_player_count", l_player_count);
    smlua_bind_function(L, "smnet_players", l_players);
    smlua_bind_function(L, "smnet_player_name", l_player_name);
    smlua_bind_function(L, "smnet_player_slot", l_player_slot);
    smlua_bind_function(L, "smnet_slot_player", l_slot_player);
    smlua_bind_function(L, "smnet_player_pos", l_player_pos);
    smlua_bind_function(L, "smnet_send", l_send);
    smlua_bind_function(L, "smnet_on_message", l_on_message);
    smlua_bind_function(L, "smnet_on_player", l_on_player);
}


/* ---- puente de la red de Lua de los mods (gPlayerSyncTable y network_send) sobre smnet
   En modo masivo cada uno es su propio servidor y los paquetes de sm64coopdx no viajan. Los de
   Lua se toman al salir (network_send / network_send_to) y van como APP_LUA:
     entradas: tipo de paquete u8, largo u16, largo de la carpeta del mod u8, carpeta, datos
   (los datos son los del paquete sin la cabecera ni el índice del mod: el índice de la lista de
   mods no coincide entre jugadores, la carpeta sí). Lo que va a todos se junta por cuadro y sale
   al área; lo que va a un índice local sale al id de quien ocupa ese slot.
   Al llegar se arma el paquete con el slot del que lo manda como origen; de quien no tiene slot
   se descarta (para un mod no existe), y al darle un slot se le pide su tabla completa
   (APP_LUA_REQ). De las tablas solo vale gPlayerSyncTable del propio emisor; gGlobalSyncTable es
   de cada uno (no hay un servidor común). */
/* Los mods mandan índices globales en sus datos (Character Select: data.index =
   network_global_index_from_local(0)). En modo masivo cada uno es servidor con índice global 0,
   así que al recibir, network_local_index_from_global(0) daba 0 (uno mismo) y el mod pisaba sus
   propios datos con los del otro; el slot del que manda nunca tenía datos (el "?" de la lista).
   Mientras se entrega un paquete de Lua de un slot, el 0 se traduce a ese slot. */
static int sLuaRxSlot = -1;
static bool sRxFromOther = false;   /* procesando un paquete del mundo que llegó de otro */
static bool sRxFromCoord = false;   /* … y el que lo manda es el coordinador */
static bool sRxGlobalAuth = false;  /* procesando un campo de gGlobalSyncTable del coordinador */
bool ns_smnet_rx_global_authority(void) { return sRxGlobalAuth; }
static bool i_am_coord(void) { smnet_node_info_t inf; smnet_node_info(sNode, &inf); return inf.id != SMNET_NO_ID && inf.id == inf.coord_id; }
static uint16_t coord_id(void) { smnet_node_info_t inf; smnet_node_info(sNode, &inf); return inf.coord_id; }

/* ¿un PACKET_LUA_SYNC_TABLE es de gGlobalSyncTable? (la última clave es el nombre de la tabla) */
static bool sync_packet_is_global(const struct Packet* src, int off) {
    static struct Packet t;
    memcpy(&t, src, sizeof t);
    t.cursor = (u16)(off + 10);                    /* seq u64 + mod u16 */
    t.error = false;
    u16 n = 0;
    packet_read(&t, &n, sizeof(u16));
    if (t.error || n < 1 || n >= MAX_UNWOUND_LNT) return false;
    struct LSTNetworkType keys[MAX_UNWOUND_LNT];
    memset(keys, 0, sizeof keys);
    bool ok = true;
    for (u16 i = 0; i < n && ok; i++) ok = packet_read_lnt(&t, &keys[i]);
    bool global = ok && keys[n - 1].type == LST_NETWORK_TYPE_STRING && keys[n - 1].value.string && !strcmp(keys[n - 1].value.string, "gGlobalSyncTable");
    for (u16 i = 0; i < n; i++) if (keys[i].type == LST_NETWORK_TYPE_STRING && keys[i].value.string) { free(keys[i].value.string); keys[i].value.string = NULL; }
    return global;
}
bool ns_smnet_rx_from_other(void) { return sRxFromOther; }
s32 ns_smnet_remap_global(u8 globalIndex) {
    if (!ns_smnet_massive() || sLuaRxSlot <= 0) return -1;
    return globalIndex == 0 ? sLuaRxSlot : -1;
}

static uint8_t sLuaArea[SMNET_MAX_APP];
static int sLuaAreaLen = 0;
static uint16_t sLuaReplyTo = SMNET_NO_ID;     /* respondiendo un pedido: todo va a este id */

static int lua_pkt_data_off(const struct Packet* p) {
    u8 fl = p->buffer[3];
    int off = 5;
    if (fl & 4) off += 5;
    if (fl & 1) off += 5; else if (fl & 8) off += 4;
    return off;
}

static uint8_t sLuaReply[SMNET_MAX_APP];
static int sLuaReplyLen = 0;
static uint8_t sLuaReplyHdr = APP_LUA;          /* APP_LUA (tabla de jugador) o APP_STATE (estado del área) */
static uint8_t sAllBuf[SMNET_MAX_APP];
static int sAllLen = 0;
static void lua_reply_flush(void) {
    if (sLuaReplyLen > 1 && sLuaReplyTo != SMNET_NO_ID &&
        smnet_node_send_msg(sNode, SMNET_TO_PLAYER, sLuaReplyTo, sLuaReply, sLuaReplyLen) == 0) sLuaRl.tx++;
    sLuaReplyLen = 0;
}
/* una entrada más grande que un mensaje (estado del área): en pedazos APP_BIG, en orden */
static void big_send_scope(smnet_scope_t scope, uint16_t to, uint8_t hdr, const uint8_t* e, int n);
static void big_send(uint16_t to, uint8_t hdr, const uint8_t* e, int n) { big_send_scope(SMNET_TO_PLAYER, to, hdr, e, n); }
static void big_send_scope(smnet_scope_t scope, uint16_t to, uint8_t hdr, const uint8_t* e, int n) {
    static uint8_t msgId = 0;
    const int chunk = SMNET_MAX_APP - 5;
    int cnt = (n + chunk - 1) / chunk;
    if (cnt > 255) { sWorld.grandes++; return; }
    msgId++;
    uint8_t b[SMNET_MAX_APP];
    for (int k = 0; k < cnt; k++) {
        int m = n - k * chunk < chunk ? n - k * chunk : chunk;
        b[0] = APP_BIG; b[1] = hdr; b[2] = msgId; b[3] = (uint8_t)k; b[4] = (uint8_t)cnt;
        memcpy(b + 5, e + k * chunk, (size_t)m);
        if (smnet_node_send_msg(sNode, scope, to, b, m + 5) != 0) { sWorld.grandes++; return; }
    }
    sWorld.partidos++;
}
static void lua_send_entry(smnet_scope_t scope, uint16_t to, const uint8_t* e, int n) {
    if (scope == SMNET_TO_PLAYER && to == sLuaReplyTo) {   /* respuesta a un pedido: se junta */
        if (n > SMNET_MAX_APP - 1) { lua_reply_flush(); big_send(to, sLuaReplyHdr, e, n); return; }
        if (sLuaReplyLen + n > SMNET_MAX_APP - 1) lua_reply_flush();
        if (sLuaReplyLen == 0) sLuaReply[sLuaReplyLen++] = sLuaReplyHdr;
        memcpy(sLuaReply + sLuaReplyLen, e, (size_t)n); sLuaReplyLen += n;
        return;
    }
    if (n > SMNET_MAX_APP - 1) {
        /* paquete grande (objetos creados de a muchos, por ejemplo): en pedazos, en orden */
        if (scope == SMNET_TO_AREA) { lua_relay_flush(); big_send_scope(SMNET_TO_AREA, 0, APP_LUA, e, n); return; }
        if (scope == SMNET_TO_ALL)  { lua_relay_flush(); big_send_scope(SMNET_TO_ALL, 0, APP_ALL, e, n); return; }
        big_send(to, APP_LUA, e, n); return;
    }
    if (scope == SMNET_TO_ALL) {
        if (sAllLen + n > SMNET_MAX_APP - 1) lua_relay_flush();
        if (sAllLen == 0) sAllBuf[sAllLen++] = APP_ALL;
        memcpy(sAllBuf + sAllLen, e, (size_t)n); sAllLen += n;
        return;
    }
    if (scope == SMNET_TO_AREA) {
        if (sLuaAreaLen + n > SMNET_MAX_APP - 1) lua_relay_flush();
        if (sLuaAreaLen == 0) sLuaArea[sLuaAreaLen++] = APP_LUA;
        memcpy(sLuaArea + sLuaAreaLen, e, (size_t)n); sLuaAreaLen += n;
        return;
    }
    uint8_t b[SMNET_MAX_APP];
    b[0] = APP_LUA; memcpy(b + 1, e, (size_t)n);
    if (smnet_node_send_msg(sNode, SMNET_TO_PLAYER, to, b, n + 1) == 0) sLuaRl.tx++;
}

static void lua_relay_flush(void) {
    if (sAllLen > 1 && smnet_node_send_msg(sNode, SMNET_TO_ALL, 0, sAllBuf, sAllLen) == 0) sLuaRl.tx++;
    sAllLen = 0;
    if (sLuaAreaLen <= 1) { sLuaAreaLen = 0; return; }
    if (smnet_node_send_msg(sNode, SMNET_TO_AREA, 0, sLuaArea, sLuaAreaLen) == 0) sLuaRl.tx++;
    sLuaAreaLen = 0;
}

/* ---- mundo compartido (etapa 2 del modo masivo): los paquetes del mundo de sm64coopdx
   (objetos, monedas, ítems, estrellas, agua, objetos de nivel destruidos) van por smnet a los de
   la misma área, enteros y sin confiabilidad propia (smnet ya es confiable salto a salto, y el
   número de secuencia de sm64coopdx es por índice local, que acá rota). El dueño de cada objeto
   lo decide sm64coopdx como siempre: el más cercano entre uno y los de slot (los bots, que no
   simulan nada, no cuentan: ns_smnet_slot_simulates). Al llegar se procesa con el slot del que lo
   manda como origen (packet_process ya descarta los de otra área); de quien no tiene slot se
   descarta: está lejos y sus objetos también. */
/* 1 = del área, 2 = de toda la partida (estrellas y guardado: todos comparten la partida, como en
   sm64coopdx), 3 = estado de nivel/área (solo como respuesta a APP_AREA_REQ) */
static int world_kind(u8 type) {
    switch (type) {
        case PACKET_OBJECT: case PACKET_SPAWN_OBJECTS: case PACKET_SPAWN_STAR: case PACKET_SPAWN_STAR_NLE:
        case PACKET_COLLECT_COIN: case PACKET_COLLECT_ITEM:
        case PACKET_CHANGE_WATER_LEVEL: case PACKET_LEVEL_MACRO: case PACKET_LEVEL_RESPAWN_INFO:
            return 1;
        case PACKET_COLLECT_STAR: case PACKET_SAVE_SET_FLAG: case PACKET_SAVE_REMOVE_FLAG:
            return 2;
        case PACKET_LEVEL: case PACKET_AREA: case PACKET_LEVEL_SPAWN_INFO:
            return 3;
        default: return 0;
    }
}
static bool world_packet(u8 type) { return world_kind(type) != 0; }

bool ns_smnet_relay_packet(struct Packet* p, u8 toLocalIndex) {
    u8 type = p->buffer[0];
    if (world_packet(type)) {
        int kind = world_kind(type);
        bool reply = sLuaReplyTo != SMNET_NO_ID;
        if (kind == 3 && !reply) return true;            /* el estado solo va como respuesta */
        int len = (int)p->dataLength;
        if (len < 5 || len > PACKET_LENGTH) { sWorld.grandes++; return true; }
        static uint8_t e[PACKET_LENGTH + 8];
        e[0] = type;
        uint16_t L = (uint16_t)len; memcpy(e + 1, &L, 2);
        memcpy(e + 3, p->buffer, (size_t)len);
        e[3 + 1] = 0; e[3 + 2] = 0;                    /* sin número de secuencia */
        e[3 + 4] = PACKET_DESTINATION_BROADCAST;
        if (reply) lua_send_entry(SMNET_TO_PLAYER, sLuaReplyTo, e, len + 3);
        else if (kind == 2) lua_send_entry(SMNET_TO_ALL, 0, e, len + 3);
        else if (toLocalIndex > 0 && toLocalIndex < MAX_PLAYERS) {
            uint16_t id = sIds[toLocalIndex];
            if (id != SMNET_NO_ID) lua_send_entry(SMNET_TO_PLAYER, id, e, len + 3);
        } else lua_send_entry(SMNET_TO_AREA, 0, e, len + 3);
        sWorld.tx++;
        return true;
    }
    int modAt;
    if (type == PACKET_LUA_CUSTOM || type == PACKET_LUA_CUSTOM_BYTESTRING) modAt = 0;
    else if (type == PACKET_LUA_SYNC_TABLE) modAt = 8;   /* después del seq (u64) */
    else return false;
    int off = lua_pkt_data_off(p);
    int dl = (int)p->dataLength - off;
    if (dl < modAt + 2) return true;
    u16 modIndex; memcpy(&modIndex, p->buffer + off + modAt, 2);
    const char* path = smlua_mod_path_from_index(modIndex);
    if (path == NULL) return true;
    int pl = (int)strlen(path); if (pl > 200) return true;
    uint8_t e[SMNET_MAX_APP];
    int n = 0, body = dl - 2;
    if (4 + pl + body > SMNET_MAX_APP - 1) { sLuaRl.malos++; return true; }   /* demasiado grande */
    e[n++] = type;
    uint16_t L = (uint16_t)(1 + pl + body); memcpy(e + n, &L, 2); n += 2;
    e[n++] = (uint8_t)pl; memcpy(e + n, path, (size_t)pl); n += pl;
    memcpy(e + n, p->buffer + off, (size_t)modAt); n += modAt;
    memcpy(e + n, p->buffer + off + modAt + 2, (size_t)(dl - modAt - 2)); n += dl - modAt - 2;
    if (sLuaReplyTo != SMNET_NO_ID) lua_send_entry(SMNET_TO_PLAYER, sLuaReplyTo, e, n);
    else if (type == PACKET_LUA_SYNC_TABLE && sync_packet_is_global(p, off)) {
        /* gGlobalSyncTable: la del coordinador va a toda la partida; la de los demás queda local
           (cada uno es servidor y la escribe; manda la del coordinador) */
        if (i_am_global_authority()) lua_send_entry(SMNET_TO_ALL, 0, e, n);
    }
    else if (toLocalIndex > 0 && toLocalIndex < MAX_PLAYERS) {
        uint16_t id = sIds[toLocalIndex];
        if (id != SMNET_NO_ID) lua_send_entry(SMNET_TO_PLAYER, id, e, n);
    } else lua_send_entry(SMNET_TO_AREA, 0, e, n);
    return true;
}

static void lua_relay_rx(uint16_t from, const uint8_t* d, int len, bool anyone, bool global) {
    int s = sSlotOf[from];
    int off = 0;
    while (off + 4 <= len) {
        u8 type = d[off];
        uint16_t L; memcpy(&L, d + off + 1, 2);
        const uint8_t* e = d + off + 3;
        off += 3 + L;
        if (off > len || L < 1) { sLuaRl.malos++; return; }
        if (world_packet(type)) {
            /* de toda la partida o respuesta de estado: vale aunque el que manda no tenga slot */
            /* sin slot también valen los que no dependen de quién los manda (monedas, ítems,
               estrellas, agua, objetos de nivel destruidos); los de objetos sí (el dueño) */
            /* los de objetos también: el dueño puede ser alguien sin slot acá (otro que simula la
               misma área y está más cerca; ver ns_smnet_other_owner_closer) */
            bool free = true;
            if (s <= 0 && !free) { sWorld.sinslot++; continue; }
            if (world_kind(type) == 3 && !anyone) { sLuaRl.malos++; continue; }
            if (L < 5 || L > PACKET_LENGTH || e[0] != type) { sLuaRl.malos++; continue; }
            struct Packet* p = &sRxPacket;
            memset(p, 0, sizeof *p);
            memcpy(p->buffer, e, L);
            p->dataLength = L;
            p->cursor = 3;
            p->localIndex = (u8)(s > 0 ? s : 0);
            sBlobFrom = from; p->addr = &sBlobFrom;
            sLuaRxSlot = s > 0 ? s : -1;
            sRxFromOther = true;
            if (packet_initial_read(p)) packet_process(p);
            sRxFromOther = false;
            sLuaRxSlot = -1;
            sWorld.rx++;
            continue;
        }
        if (1 + e[0] > L) { sLuaRl.malos++; return; }
        if (s <= 0 && !global) { sLuaRl.sinslot++; continue; }
        char path[256]; int pl = e[0];
        memcpy(path, e + 1, (size_t)pl); path[pl] = 0;
        int mi = smlua_mod_index_from_path(path);
        if (mi < 0) { sLuaRl.malos++; continue; }     /* no tenemos ese mod */
        const uint8_t* body = e + 1 + pl; int bl = L - 1 - pl;
        int modAt = type == PACKET_LUA_SYNC_TABLE ? 8 : 0;
        if (bl < modAt) { sLuaRl.malos++; continue; }
        struct Packet* p = &sRxPacket;
        packet_init(p, (enum PacketType)type, false, PLMT_NONE);
        u16 m16 = (u16)mi;
        packet_write(p, (void*)body, (u16)modAt);
        packet_write(p, &m16, 2);
        packet_write(p, (void*)(body + modAt), (u16)(bl - modAt));
        p->cursor = (u16)lua_pkt_data_off(p);
        p->localIndex = (u8)(s > 0 ? s : 0);
        sBlobFrom = from; p->addr = &sBlobFrom;
        sLuaRxSlot = s > 0 ? s : -1;    /* mientras corre el mod: el índice global 0 del que manda es su slot */
        sRxFromCoord = global && from == global_authority_id();
        if (type == PACKET_LUA_SYNC_TABLE) network_receive_lua_sync_table(p);
        else if (type == PACKET_LUA_CUSTOM && s > 0) network_receive_lua_custom(p);
        else if (type == PACKET_LUA_CUSTOM_BYTESTRING && s > 0) network_receive_lua_custom_bytestring(p);
        sRxFromCoord = false; sRxGlobalAuth = false;
        sLuaRxSlot = -1;
        sLuaRl.rx++;
    }
}

bool ns_smnet_lua_sync_keys(u8 fromLocalIndex, u16 n, struct LSTNetworkType* keys) {
    if (n < 2) return false;
    struct LSTNetworkType* t = &keys[n - 1];
    if (t->type != LST_NETWORK_TYPE_STRING || t->value.string == NULL) return false;
    if (!strcmp(t->value.string, "gGlobalSyncTable")) {
        /* solo la del coordinador, y se acepta aunque acá se haya escrito antes */
        if (!sRxFromCoord) return false;
        sRxGlobalAuth = true;
        return true;
    }
    if (fromLocalIndex == 0 || fromLocalIndex >= MAX_PLAYERS) return false;
    if (strcmp(t->value.string, "gPlayerSyncTable") != 0) return false;
    if (n < 3 || keys[n - 2].type != LST_NETWORK_TYPE_INTEGER) return false;
    if (keys[n - 2].value.integer != 0) return false;   /* solo la propia (índice global 0 del que manda) */
    keys[n - 2].value.integer = gNetworkPlayers[fromLocalIndex].globalIndex;
    return true;
}

/* un slot cambia de jugador: se vacía su tabla y se le pide la suya al nuevo */
static void lua_slot_changed(int s, uint16_t id) {
    if (gLuaState == NULL) return;
    smlua_sync_table_clear_player((u8)s);
    if (id == SMNET_NO_ID) return;
    uint8_t b = APP_LUA_REQ;
    if (smnet_node_send_msg(sNode, SMNET_TO_PLAYER, id, &b, 1) == 0) sLuaRl.pedidos++;
}

/* ---- estado del nivel y del área para el que llega (mundo compartido, versión 2)
   Al entrar a un área se le pide a uno que ya esté ahí (el más cercano que simula) su estado:
   sm64coopdx lo arma igual que cuando un cliente entra a un área (network_send_level con el área:
   variables del nivel, objetos de macro y de spawn destruidos, objetos borrados, respawners,
   objetos creados en el momento y el último paquete de cada objeto). Va como APP_STATE, con los
   paquetes grandes en pedazos APP_BIG. Si no llega en 3 s se le pide a otro (hasta 3 veces). */
static struct { s16 level; s8 area, act, course; double since, asked; int tries; bool got; } sAreaSt = { -1, -1, -1, -1, 0, 0, 0, false };
static uint16_t sAreaAskedTo = SMNET_NO_ID;
static void area_state_got(void) { if (!sAreaSt.got) { sAreaSt.got = true; smlog("estado del área recibido"); } }

static void world_area_update(double now) {
    extern s16 gCurrCourseNum, gCurrActStarNum, gCurrLevelNum, gCurrAreaIndex;
    if (gCurrLevelNum != sAreaSt.level || gCurrAreaIndex != sAreaSt.area || gCurrActStarNum != sAreaSt.act || gCurrCourseNum != sAreaSt.course) {
        sAreaSt.level = gCurrLevelNum; sAreaSt.area = (s8)gCurrAreaIndex; sAreaSt.act = (s8)gCurrActStarNum; sAreaSt.course = (s8)gCurrCourseNum;
        sAreaSt.since = now; sAreaSt.asked = 0; sAreaSt.tries = 0; sAreaSt.got = false; sAreaAskedTo = SMNET_NO_ID;
        return;
    }
    if (sAreaSt.got || sAreaSt.tries >= 3 || !gNetworkAreaLoaded) return;
    if (now - sAreaSt.since < 700) return;                       /* que lleguen los vecinos del área */
    if (sAreaSt.asked > 0 && now - sAreaSt.asked < 3000) return;
    /* el más cercano que simula (no bots) y no se le pidió ya */
    smnet_node_info_t inf; smnet_node_info(sNode, &inf);
    struct MarioState* m = &gMarioStates[0];
    uint16_t best = SMNET_NO_ID; float bd = 1e30f;
    for (int id = 0; id < SMNET_MAX_PLAYERS; id++) {
        if (id == inf.id || id == sAreaAskedTo || sEnt[id].seen <= 0 || now - sEnt[id].seen > 1500) continue;
        s16 lv; u8 ar, ac;
        if (!info_location((uint16_t)id, &lv, &ar, &ac) || lv != gCurrLevelNum || ar != (u8)gCurrAreaIndex || ac != (u8)gCurrActStarNum) continue;
        float dx = sEnt[id].x - m->pos[0], dy = sEnt[id].y - m->pos[1], dz = sEnt[id].z - m->pos[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d < bd) { bd = d; best = (uint16_t)id; }
    }
    if (best == SMNET_NO_ID) { if (now - sAreaSt.since > 5000) sAreaSt.tries = 3; return; }   /* solo en el área */
    uint8_t b[7];
    s16 lv = gCurrLevelNum;
    b[0] = APP_AREA_REQ; b[1] = (uint8_t)gCurrCourseNum; b[2] = (uint8_t)gCurrActStarNum;
    memcpy(b + 3, &lv, 2); b[5] = (uint8_t)gCurrAreaIndex; b[6] = 0;
    if (smnet_node_send_msg(sNode, SMNET_TO_PLAYER, best, b, 6) == 0) { sWorld.pedidos_area++; smlog("estado del área pedido a smnet:%u", best); }
    sAreaAskedTo = best; sAreaSt.asked = now; sAreaSt.tries++;
}

static void world_area_request_rx(uint16_t from, const uint8_t* d, int len) {
    extern s16 gCurrCourseNum, gCurrActStarNum, gCurrLevelNum, gCurrAreaIndex;
    if (len < 5) return;
    s16 lv; memcpy(&lv, d + 2, 2);
    if (d[0] != (uint8_t)gCurrCourseNum || d[1] != (uint8_t)gCurrActStarNum || lv != gCurrLevelNum || d[4] != (uint8_t)gCurrAreaIndex) return;
    if (!gNetworkAreaLoaded || gNetworkPlayerLocal == NULL || !gNetworkPlayerLocal->currAreaSyncValid) return;
    /* destinatario ficticio: network_send_sync_valid marca al destinatario como sincronizado, y eso
       no debe tocar a ningún slot; todo lo que se manda va a `from` (sLuaReplyTo) */
    static struct NetworkPlayer dummy;
    memset(&dummy, 0, sizeof dummy);
    dummy.connected = true; dummy.type = NPT_CLIENT; dummy.localIndex = 1; dummy.globalIndex = 1;
    lua_relay_flush();
    sLuaReplyTo = from; sLuaReplyHdr = APP_STATE; sLuaReplyLen = 0;
    network_send_level(&dummy, true);
    lua_reply_flush();
    sLuaReplyTo = SMNET_NO_ID; sLuaReplyHdr = APP_LUA;
    sWorld.respuestas_area++;
    smlog("estado del área enviado a smnet:%u", from);
}

/* pedazos APP_BIG de cada uno: se juntan y se procesan como un mensaje de ese tipo */
static struct { uint8_t* buf; int len; uint8_t id, next, cnt, hdr; } *sBig;
static void world_big_rx(uint16_t from, const uint8_t* d, int len) {
    if (len < 4 || from >= SMNET_MAX_PLAYERS) return;
    if (!sBig) { sBig = calloc(SMNET_MAX_PLAYERS, sizeof *sBig); if (!sBig) return; }
    uint8_t hdr = d[0], id = d[1], idx = d[2], cnt = d[3];
    if (idx == 0) { sBig[from].id = id; sBig[from].next = 0; sBig[from].len = 0; sBig[from].cnt = cnt; sBig[from].hdr = hdr; }
    if (sBig[from].id != id || sBig[from].next != idx) return;   /* falta un pedazo: se descarta */
    if (!sBig[from].buf) { sBig[from].buf = malloc(PACKET_LENGTH + 16); if (!sBig[from].buf) return; }
    if (sBig[from].len + len - 4 > PACKET_LENGTH + 16) return;
    memcpy(sBig[from].buf + sBig[from].len, d + 4, (size_t)(len - 4));
    sBig[from].len += len - 4;
    sBig[from].next++;
    if (sBig[from].next < sBig[from].cnt) return;
    lua_relay_rx(from, sBig[from].buf, sBig[from].len, sBig[from].hdr == APP_STATE, sBig[from].hdr == APP_ALL);
    if (sBig[from].hdr == APP_STATE) area_state_got();
}

static void lua_answer_global(uint16_t from) {
    if (gLuaState == NULL || !(i_am_global_authority() || i_am_coord())) return;
    lua_relay_flush();
    sLuaReplyTo = from; sLuaReplyHdr = APP_ALL; sLuaReplyLen = 0;
    smlua_sync_table_send_own_global();
    lua_reply_flush();
    sLuaReplyTo = SMNET_NO_ID; sLuaReplyHdr = APP_LUA;
    smlog("gGlobalSyncTable enviada a smnet:%u", from);
}

/* al entrar (y si cambia el coordinador) se le pide gGlobalSyncTable */
static void lua_global_update(void) {
    static uint16_t askedCoord = SMNET_NO_ID;
    smnet_node_info_t inf; smnet_node_info(sNode, &inf);
    if (inf.status != SMNET_PEER_CONNECTED) { askedCoord = SMNET_NO_ID; return; }
    if (sOrigHost && host_ready()) return;      /* el host manda la suya */
    uint16_t to = global_authority_id();        /* el host si está; si no, el coordinador */
    if (to == SMNET_NO_ID || to == inf.id || to == askedCoord) return;
    if (gLuaState == NULL) return;
    uint8_t b = APP_GLOBAL_REQ;
    if (smnet_node_send_msg(sNode, SMNET_TO_PLAYER, to, &b, 1) == 0) { askedCoord = to; smlog("gGlobalSyncTable pedida a smnet:%u%s", to, to == sHostId ? " (el host)" : " (coordinador)"); }
}

static void lua_answer_request(uint16_t from) {
    if (gLuaState == NULL) return;
    lua_relay_flush();
    sLuaReplyTo = from;
    sLuaReplyLen = 0;
    smlua_sync_table_send_own_player();
    lua_reply_flush();
    sLuaReplyTo = SMNET_NO_ID;
    sLuaRl.respuestas++;
}

static uint16_t own_model_ext(void) {
    struct Object* o = gMarioStates[0].marioObj;
    if (o == NULL) return 0;
    return (uint16_t)obj_get_model_id_extended(o);
}

/* modelo para un impostor: el que anunció (Character Select y parecidos), si no el del personaje */
static s32 imp_model_for_ext(uint16_t ext, u8 model) {
    if (ext == 0 || ext == E_MODEL_NONE) return (s32)gCharacters[model].modelId;
    return (s32)smlua_model_util_load((enum ModelExtendedId)ext);
}
#else
static void lua_player_event(uint16_t id, int joined) { (void)id; (void)joined; }
static void lua_mod_message(uint16_t from, const uint8_t* d, int len) { (void)from; (void)d; (void)len; }
static void lua_slot_changed(int s, uint16_t id) { (void)s; (void)id; }
static void lua_relay_rx(uint16_t from, const uint8_t* d, int len, bool anyone, bool global) { (void)from; (void)d; (void)len; (void)anyone; (void)global; }
static void world_big_rx(uint16_t from, const uint8_t* d, int len) { (void)from; (void)d; (void)len; }
static void world_area_request_rx(uint16_t from, const uint8_t* d, int len) { (void)from; (void)d; (void)len; }
static void world_area_update(double now) { (void)now; }
static void area_state_got(void) {}
bool ns_smnet_rx_from_other(void) { return false; }
static void lua_relay_flush(void) {}
static void lua_answer_request(uint16_t from) { (void)from; }
static void lua_answer_global(uint16_t from) { (void)from; }
static void lua_global_update(void) {}
bool ns_smnet_rx_global_authority(void) { return false; }
static uint16_t own_model_ext(void) { return 0; }
s32 ns_smnet_remap_global(u8 globalIndex) { (void)globalIndex; return -1; }
bool ns_smnet_relay_packet(struct Packet* p, u8 toLocalIndex) { (void)p; (void)toLocalIndex; return false; }
bool ns_smnet_lua_sync_keys(u8 fromLocalIndex, u16 n, struct LSTNetworkType* keys) { (void)fromLocalIndex; (void)n; (void)keys; return false; }
static s32 imp_model_for_ext(uint16_t ext, u8 model) { (void)ext; return (s32)gCharacters[model].modelId; }
#endif

/* el PLAYER de otro, que llegó por el flujo de estado */
static void on_blob(void* user, uint16_t id, uint8_t seq, const uint8_t* blob, int len, double now) {
    (void)user; (void)seq; (void)now;
    uint8_t gi;
    uint16_t ak;
    if (sMassive) { if (id < SMNET_MAX_PLAYERS && ns_smnet_mod_hidden(id)) return; if (id < SMNET_MAX_PLAYERS) { sEnt[id].seen = now; sBlobAt[id] = now; } massive_inject(id, blob, len); return; }
    if (!player_pkt_info(blob, len, &gi, &ak)) { sPl.malo++; return; }
    if (gNetworkPlayerLocal == NULL || gi == gNetworkPlayerLocal->globalIndex) return;
    struct NetworkPlayer* np = network_player_from_global_index(gi);
    if (np == NULL || !np->connected || np->localIndex == 0 || np->localIndex >= MAX_PLAYERS) { sPl.desconocido++; return; }
    /* el host sabe qué id de smnet tiene cada índice: un estado que dice ser de otro se descarta */
    if (gNetworkType == NT_SERVER && sIds[np->localIndex] != id) { sPl.ajeno++; return; }
    sBlobFrom = id;
    network_receive(np->localIndex, &sBlobFrom, (u8*)blob, (u16)len);
    sPl.rx++;
}

static void log_status(const char* why) {
    if (!sNode) return;
    smnet_node_info_t inf;
    smnet_node_info(sNode, &inf);
    const smnet_stats_t* st = smnet_node_stats(sNode);
    const smnet_link_stats_t* ls = smnet_node_link_stats(sNode);
    smlog("%s: estado=%d id=%u coord=%u rol_coord=%d sn=%d sesiones=%d hs_ok=%llu hs_fallidos=%llu datos_tx=%llu datos_rx=%llu app_tx=%llu app_rx=%llu tasa=%llu no_autorizados=%llu pendientes=%d",
          why, (int)inf.status, inf.id, inf.coord_id, (int)inf.coord_role, inf.is_sn, ls->sessions,
          (unsigned long long)ls->hs_ok, (unsigned long long)ls->hs_failed, (unsigned long long)ls->data_tx, (unsigned long long)ls->data_rx,
          (unsigned long long)st->app_tx, (unsigned long long)st->app_rx, (unsigned long long)st->app_rate_limited,
          (unsigned long long)st->unauthorized, sNumPending);
    smlog("  lista de la partida: %d jugadores (contándose)", sKnownCount + 1);
    smlog("  PLAYER por estado: publicados=%llu recibidos=%llu (blobs=%llu) sin_jugador=%llu ajenos=%llu grandes=%llu malos=%llu",
          sPl.tx, sPl.rx, (unsigned long long)st->blob_updates, sPl.desconocido, sPl.ajeno, sPl.grande, sPl.malo);
    if (sMassive) {
        int used = 0;
        for (int s = 1; s < MAX_PLAYERS; s++) used += sIds[s] != SMNET_NO_ID;
        smlog("  slots: ocupados=%d/%d asignados=%llu cambios=%llu liberados=%llu cercanos_sin_slot=%llu entidades=%llu area_propia_valida=%d",
              used, MAX_PLAYERS - 1, sSl.asignados, sSl.cambios, sSl.liberados, sSl.sin_slot, (unsigned long long)st->entity_updates,
              gNetworkPlayerLocal ? (int)gNetworkPlayerLocal->currAreaSyncValid : -1);
        { int ni = 0, nt[3] = { 0, 0, 0 }; for (int i = 0; i < IMP_MAX; i++) if (sImp[i].o) { ni++; nt[sImp[i].tier]++; }
          smlog("  impostores: %d (máx %d): medios %d/%d, bajos %d/%d%s, lejanos %d%s%s; creados en total %llu, cambios de nivel %llu",
                ni, sImpMax, nt[0], sImpMario, nt[1], sImpBajo, sImpBajoCompleto ? " (completos)" : " (pocas caras)", nt[2],
                sImpGorra ? " (gorra)" : sImpLejCompleto ? " (completos)" : " (pocas caras)", sImpLejQuietos ? ", quietos" : ", animados a 5 Hz", sImpShown, sImpTierChg); }
        smlog("  lua de los mods: enviados %llu, recibidos %llu, de jugadores sin slot %llu, pedidos de tabla %llu, respondidos %llu, malos %llu",
              sLuaRl.tx, sLuaRl.rx, sLuaRl.sinslot, sLuaRl.pedidos, sLuaRl.respuestas, sLuaRl.malos);
        if (sGameSeeder) { ms_seeder_stats_t ss; ms_seeder_stats(sGameSeeder, &ss);
          smlog("  mods: manifiesto pedido %llu veces, repartido %.1f MB (%llu pedazos), atendiendo %d, en espera %d", ss.pedidos_manifiesto, ss.bytes / 1048576.0, ss.pedazos, ss.atendiendo, ss.en_espera); }
        smlog("  mundo: enviados %llu, recibidos %llu, de jugadores sin slot %llu, demasiado grandes %llu, en pedazos %llu, estado de área pedido %llu / enviado %llu",
              sWorld.tx, sWorld.rx, sWorld.sinslot, sWorld.grandes, sWorld.partidos, sWorld.pedidos_area, sWorld.respuestas_area);
        smlog("  chat: enviados=%llu recibidos=%llu descartados=%llu", sChatTx, sChatRx, sChatDrop);
        for (int s = 1; s < MAX_PLAYERS; s++) if (sIds[s] != SMNET_NO_ID)
            smlog("    slot %d: smnet:%u \"%s\" nivel %d área %d", s, sIds[s], gNetworkPlayers[s].name, gNetworkPlayers[s].currLevelNum, gNetworkPlayers[s].currAreaIndex);
    }
}

bool ns_smnet_wanted(void) {
    const char* e = getenv("COOPDX_SMNET");
    return !(e && e[0] == '0');
}

static void on_send(void* user, const smnet_addr_t* to, const uint8_t* d, int len) {
    (void)user;
    if (sFd >= 0) smnet_udp_send(sFd, to, d, len);
}

static void massive_chat_rx(uint16_t from, const uint8_t* d, int len);

static void on_msg(void* user, uint16_t from, smnet_scope_t scope, const uint8_t* d, int len, double now) {
    (void)user; (void)scope; (void)now;
    if (len < 1 || from >= SMNET_MAX_PLAYERS) return;
    if (sMassive) {
        if (d[0] >= MS_MSG_FIRST && d[0] <= MS_MSG_LAST) {   /* descarga de mods: de los que se unen */
            if (sGameSeeder) ms_seeder_on_msg(sGameSeeder, from, d, len, now);
            return;
        }
        if (d[0] == APP_MODLIST || d[0] == APP_KICKED || d[0] == APP_MODREQ) { ns_smnet_mod_rx(from, d, len, now); return; }
        if (ns_smnet_mod_hidden(from)) return;   /* echado o baneado: nada de lo suyo */
        if (d[0] == APP_CHAT) { if (!ns_smnet_mod_muted(from)) massive_chat_rx(from, d + 1, len - 1); }
        else if (d[0] == APP_MOD) lua_mod_message(from, d + 1, len - 1);
        else if (d[0] == APP_LUA) lua_relay_rx(from, d + 1, len - 1, false, false);
        else if (d[0] == APP_STATE || d[0] == APP_ALL) { lua_relay_rx(from, d + 1, len - 1, d[0] == APP_STATE, d[0] == APP_ALL); if (d[0] == APP_STATE) area_state_got(); }
        else if (d[0] == APP_GLOBAL_REQ) lua_answer_global(from);
        else if (d[0] == APP_BIG) world_big_rx(from, d + 1, len - 1);
        else if (d[0] == APP_AREA_REQ) world_area_request_rx(from, d + 1, len - 1);
        else if (d[0] == APP_LUA_REQ) lua_answer_request(from);
        else if (d[0] == APP_HOST) host_claim_rx(from, d + 1, len - 1);
        else if (d[0] == APP_END && from == sHostId && !sOrigHost) { sEndRx = true; smlog("el host finalizó la partida"); }
        return;
    }
    uint8_t fl = d[0];
    if (fl & FRAG_FIRST) sRe[from].len = 0;
    if (!sRe[from].data) { sRe[from].data = malloc(PACKET_LENGTH + 1); if (!sRe[from].data) return; }
    if (sRe[from].len + (len - 1) > PACKET_LENGTH) { sRe[from].len = 0; return; }   /* demasiado grande */
    memcpy(sRe[from].data + sRe[from].len, d + 1, (size_t)(len - 1));
    sRe[from].len = (uint16_t)(sRe[from].len + len - 1);
    if (!(fl & FRAG_LAST)) return;
    u8 localIndex = UNKNOWN_LOCAL_INDEX;
    for (int i = 1; i < MAX_PLAYERS; i++) if (sIds[i] == from) { localIndex = (u8)i; break; }
    sIds[0] = from;
    smlog("recibido de smnet:%u: %u B, %s, índice %d", from, sRe[from].len, pkt_name(sRe[from].data, sRe[from].len), localIndex);
    network_receive(localIndex, &sIds[0], sRe[from].data, sRe[from].len);
    sRe[from].len = 0;
}

static int send_frags(uint16_t to, const uint8_t* data, int len) {
    uint8_t buf[SMNET_MAX_APP];
    int off = 0;
    do {
        int n = len - off > FRAG_DATA ? FRAG_DATA : len - off;
        buf[0] = (uint8_t)((off == 0 ? FRAG_FIRST : 0) | (off + n == len ? FRAG_LAST : 0));
        memcpy(buf + 1, data + off, (size_t)n);
        int rc = smnet_node_send_msg(sNode, SMNET_TO_PLAYER, to, buf, n + 1);
        if (rc != 0) { smlog("  smnet_node_send_msg = %d (pedazo en %d de %d)", rc, off, len); return -1; }
        off += n;
    } while (off < len);
    return 0;
}

static void flush_pending(void) {
    smnet_node_info_t inf;
    smnet_node_info(sNode, &inf);
    if (inf.status != SMNET_PEER_CONNECTED) return;
    for (int i = 0; i < sNumPending; i++) {
        if (sPending[i].to == SMNET_NO_ID) sPending[i].to = inf.coord_id;   /* "al host" antes de saber su id */
        send_frags(sPending[i].to, sPending[i].data, sPending[i].len);
        free(sPending[i].data);
    }
    sNumPending = 0;
}

/* el nodo de smnet: como host (coordinador) o para entrar a una partida */
static smnet_node_t* node_make(bool asHost, unsigned port) {
    smnet_node_cfg_t c;
    smnet_node_cfg_default(&c);
    if (smnet_random(c.seed, 32) != 0) { LOG_ERROR("smnet: sin entropía del sistema"); return NULL; }
    identity_seed_load(c.identity_seed);   /* la identidad dura entre partidas: los baneos se aplican a ella */
    memcpy(&c.nonce, c.seed + 24, 4);
    /* subida declarada: decide quién puede ser SN (hace falta >= 20000 para que un cliente lo sea;
       con 10000 el host queda como único SN y la partida se llena en ~20) */
    const char* up = getenv("COOPDX_SMNET_SUBIDA");
    /* Ajustes → "Mi conexión: lenta": 10000, no lo eligen de SN (no reparte a otros) */
    c.upload_kbps = (up && atoi(up) > 0) ? (uint32_t)atoi(up) : configSmnetUpload == 1 ? 10000 : 30000;
    c.app_rate_direct = 3000;   /* el resto del tráfico de sm64coopdx va "a un jugador" */
    /* estado rico (el PLAYER de sm64coopdx, ~120-200 B) a los MAX_PLAYERS-1 más cercanos de la
       área, sin límite de distancia hasta que existan los impostores */
    c.sn.full_k = SLOT_FULL_K;   /* 15 slots + margen para la histéresis */
    c.sn.full_radius = 1e7f;
    c.sn.full_budget_kbps = 1200;
    /* tasas por distancia: 30 Hz a los 64 más cercanos (slots + impostores de cerca), 10 Hz
       hasta el 176 (impostores de lejos), 2 Hz al resto. ~400 kbit/s de bajada por jugador. */
    c.sn.tier_k[0] = 64;
    c.sn.tier_k[1] = 176;
    c.sn.down_budget_kbps = 900;
    if (asHost) {
        c.is_host = 1;
        c.nat = SMNET_NAT_OPEN;
        char ip[16];
        const char* e = getenv("COOPDX_SMNET_IP");   /* IP pública si se hostea por Internet */
        if (e && e[0]) snprintf(ip, sizeof ip, "%s", e); else smnet_udp_lan_ip(ip);
        smnet_addr_parse(ip, (uint16_t)port, &c.self_addr);
        LOG_INFO("smnet: host en %s:%u", ip, port);
        smlog("host en %s:%u", ip, port);
    } else {
        /* el tipo de NAT que midió el Join por coopnet (si no, de cono): un simétrico no es SN */
        int h = ns_smnet_nat_hint();
        c.nat = h == NS_NAT_OPEN ? SMNET_NAT_OPEN : h == NS_NAT_SYMMETRIC ? SMNET_NAT_SYMMETRIC : SMNET_NAT_CONE;
    }
    smnet_node_t* nd = smnet_node_new(&c, on_send, NULL);
    if (!nd) { LOG_ERROR("smnet: no se pudo crear el nodo"); return NULL; }
    smnet_node_set_app_callbacks(nd, on_msg, NULL, NULL);
    smnet_node_set_callbacks(nd, sMassive ? on_entity : NULL, on_blob);
    if (sMassive) smnet_node_set_app_callbacks(nd, on_msg, on_player, NULL);
    return nd;
}

static bool ns_smnet_initialize(enum NetworkType networkType, UNUSED bool reconnecting) {
    unsigned int port = (networkType == NT_CLIENT) ? configJoinPort : configHostPort;
    if (port == 0) { port = DEFAULT_PORT; }
    /* modo masivo: todos le dan "Host"; los que se suman a una partida ajena dicen a cuál */
    /* diseño final: Host abre una partida masiva y Join (por IP) se suma a una: el que se une
       también arranca su mundo como "servidor" propio y entra a la red de smnet del otro
       (ns_smnet_request_massive_join, desde djui_panel_join_direct.c). COOPDX_SMNET_MASIVO=0
       vuelve al modo clásico de hasta 16; COOPDX_SMNET_UNIRSE=ip[:puerto] sigue valiendo. */
    static char reqCopy[96];
    snprintf(reqCopy, sizeof reqCopy, "%s", sReqJoin);
    sReqJoin[0] = '\0';                 /* un Host posterior es Host (no repite la unión) */
    const char* join = getenv("COOPDX_SMNET_UNIRSE");
    if ((!join || !join[0]) && reqCopy[0]) join = reqCopy;
    sMassiveJoin = (networkType == NT_SERVER && join && join[0]);
    sMassive = sMassiveJoin || (networkType == NT_SERVER && ns_smnet_massive_default());
    sJoinState = sMassiveJoin ? 1 : 0; sJoinSince = smnet_now_ms();
    if (sMassiveJoin) snprintf(sJoinTarget, sizeof sJoinTarget, "%s", join);
    char joinIp[96] = { 0 };
    unsigned joinPort = DEFAULT_PORT;
    if (sMassiveJoin) {
        snprintf(joinIp, sizeof joinIp, "%s", join);
        char* c = strrchr(joinIp, ':');
        if (c) { *c = '\0'; joinPort = (unsigned)atoi(c + 1); if (joinPort == 0) joinPort = DEFAULT_PORT; }
    }
    memset(sEnt, 0, sizeof sEnt); memset(sSlotOf, 0, sizeof sSlotOf); memset(sSlotMade, 0, sizeof sSlotMade);
    memset(sInfo, 0, sizeof sInfo); memset(&sSl, 0, sizeof sSl); sLastSlots = 0;
    for (int i = 0; i < IMP_MAX; i++) { sImp[i].o = NULL; sImp[i].id = SMNET_NO_ID; }
    memset(sImpOf, 0xFF, sizeof sImpOf);
    sImpLevel = sImpArea = sImpAct = -1; sLastImp = 0; sImpShown = 0; sImpTierChg = 0; sImpFrame = 0;
    sChatTx = sChatRx = sChatDrop = 0;
    memset(&sLuaRl, 0, sizeof sLuaRl); memset(&sWorld, 0, sizeof sWorld);   /* contadores del log: por sesión */
    memset(sKnown, 0, sizeof sKnown); sKnownCount = 0;
    if (!reconnecting) memset(&sTel, 0, sizeof sTel);   /* la telemetría sigue a través de las reconexiones */
    sImpEnvMed = sImpEnvBajo = sImpEnvMax = -1;
    { const char* e = getenv("COOPDX_SMNET_IMPOSTORES"); if (e && e[0]) { sImpEnvMax = atoi(e); if (sImpEnvMax < 0) sImpEnvMax = 0; } }
    { const char* e = getenv("COOPDX_SMNET_IMPOSTORES_MARIO"); if (e && e[0]) { sImpEnvMed = atoi(e); if (sImpEnvMed < 0) sImpEnvMed = 0; } }
    { const char* e = getenv("COOPDX_SMNET_IMPOSTORES_BAJOS"); if (e && e[0]) { sImpEnvBajo = atoi(e); if (sImpEnvBajo < 0) sImpEnvBajo = 0; } }
    imp_read_config();
    ns_smnet_options_apply();
    bool isHost = (networkType == NT_SERVER && !sMassiveJoin);
    /* el host de la partida: su clave firma la lista de mods y su "soy el host"; si dejó una
       partida andando (salió sin finalizarla), vuelve a entrar a esa como host */
    sOrigHost = sMassive && isHost;
    if (!reconnecting) ns_smnet_mod_reset(sOrigHost);   /* las listas siguen a través de las reconexiones */
    sHostId = SMNET_NO_ID; sClaimedAs = SMNET_NO_ID; sHostConnAt = 0; sEndAt = 0; sEndRx = sEnding = false;
    sRejoinN = sRejoinIdx = 0; memset(sPeers, 0, sizeof sPeers); sStateSaved = 0; sHostPort = port;
    sGameModsTried = -1e9; sOwnCandFrame = 0xFFFFFFFFu;
    if (sOrigHost) {
        host_key_load();
        sGamePubOk = sHostKeyOk; if (sHostKeyOk) memcpy(sGamePub, sHostPub, 32);
        sGameSigOk = false;
        if (!getenv("COOPDX_SMNET_PARTIDA_NUEVA")) sRejoinN = game_state_load(sRejoin, REJOIN_MAX);
    }
    if (!sLog) {
        /* COOPDX_SMNET_LOG=archivo: para varias instancias en la misma carpeta (abrir-instancias.sh) */
        const char* lp = getenv("COOPDX_SMNET_LOG");
        sLog = fopen((lp && lp[0]) ? lp : (isHost ? "smnet-host.log" : "smnet-cliente.log"), "a");
    }
    smlog("---- inicio (%s%s), puerto %u", isHost ? "host" : "cliente", sMassive ? ", modo masivo" : "", sMassiveJoin ? joinPort : port);
    sFd = smnet_udp_open(isHost ? (uint16_t)port : 0);
    if (sFd < 0) { LOG_ERROR("smnet: no se pudo abrir el puerto UDP %u", port); smlog("no se pudo abrir el puerto UDP %u", port); return false; }

    sNode = node_make(isHost && sRejoinN == 0, port);
    if (!sNode) { smnet_udp_close(sFd); sFd = -1; return false; }
    if (sMassive) publish_info();
    memset(&sPl, 0, sizeof sPl);
    for (int i = 0; i < MAX_PLAYERS; i++) sIds[i] = SMNET_NO_ID;
    double now = smnet_now_ms();
    if (sMassiveJoin) {
        smnet_addr_t coord;
        if (smnet_addr_parse(joinIp, (uint16_t)joinPort, &coord) != 0) { LOG_ERROR("smnet: no resuelvo %s", joinIp); return false; }
        smnet_node_start(sNode, &coord, now);
        sJoinAddr = coord; sJoinAddrOk = true; sLostAt = 0;
        LOG_INFO("smnet: modo masivo, uniéndose a %s:%u", joinIp, joinPort);
        smlog("modo masivo: uniéndose a %s:%u", joinIp, joinPort);
    } else if (networkType == NT_CLIENT) {
        smnet_addr_t coord;
        if (smnet_addr_parse(configJoinIp, (uint16_t)port, &coord) != 0) { LOG_ERROR("smnet: no resuelvo %s", configJoinIp); return false; }
        smnet_node_start(sNode, &coord, now);
        LOG_INFO("smnet: conectando a %s:%u", configJoinIp, port);
        smlog("conectando a %s:%u", configJoinIp, port);
        /* igual que socket.c: el transporte arranca la conversación del cliente (el pedido de la
           lista de mods; queda en espera hasta que smnet termine de entrar) */
        djui_connect_menu_open();
        gNetworkType = NT_CLIENT;
        network_send_mod_list_request();
        smlog("pedido de la lista de mods enviado (primer paquete del cliente)");
    } else if (sRejoinN > 0) {
        smnet_node_start(sNode, &sRejoin[0], now);
        sRejoinAt = now;
        smlog("host: volviendo a la partida que dejó andando (%d direcciones)", sRejoinN);
        djui_chat_message_create("\\#ffc080\\smnet: volviendo a tu partida como host…");
    } else {
        smnet_node_start(sNode, NULL, now);
    }
    if (sOrigHost && ns_smnet_lobby_host_requested()) ns_smnet_lobby_host_start();   /* listada en coopnet */
    return true;
}

/* volver a entrar de cero (nodo nuevo): los que se conocían se dan por idos */
static void rejoin_fresh(double now) {
    for (int id = 0; id < SMNET_MAX_PLAYERS; id++) if (sKnown[id]) on_player(NULL, (uint16_t)id, 0, NULL, 0);
    smnet_node_free(sNode);
    sNode = node_make(false, 0);
    if (!sNode) return;
    publish_info();
    smnet_node_start(sNode, &sJoinAddr, now);
    sHostId = SMNET_NO_ID;
    smlog("sin conexión hace rato: se vuelve a entrar de cero a %s", sJoinTarget);
}

/* el host que vuelve: prueba cada dirección guardada 2,5 s; si ninguna contesta (en una partida chica
   no hay quien tome el control y los demás se quedan reintentando con él), abre una partida nueva en
   su puerto, y los que estaban vuelven a entrar */
static void rejoin_update(double now) {
    if (sRejoinN == 0) return;
    smnet_node_info_t inf; smnet_node_info(sNode, &inf);
    if (inf.status == SMNET_PEER_CONNECTED) {
        sRejoinN = 0;
        smlog("host: de vuelta en la partida (coordinador smnet:%u)", inf.coord_id);
        djui_chat_message_create("\\#a0ffa0\\smnet: volviste a tu partida; seguís siendo el host");
        return;
    }
    if (now - sRejoinAt < 2500) return;
    smnet_node_free(sNode);
    sRejoinIdx++;
    bool fresh = sRejoinIdx >= sRejoinN;
    sNode = node_make(fresh, sHostPort);
    if (!sNode) { sRejoinN = 0; return; }
    publish_info();
    sRejoinAt = now;
    if (fresh) {
        sRejoinN = 0;
        smnet_node_start(sNode, NULL, now);
        smlog("host: la partida anterior ya no está; partida nueva");
        djui_chat_message_create("\\#ffc080\\smnet: tu partida anterior ya terminó; abriste una nueva");
    } else smnet_node_start(sNode, &sRejoin[sRejoinIdx], now);
}

static s64 ns_smnet_get_id(u8 localId) { return localId < MAX_PLAYERS ? (s64)sIds[localId] : -1; }

static char* ns_smnet_get_id_str(u8 localId) {
    static char s[24];
    if (localId == UNKNOWN_LOCAL_INDEX) { localId = 0; }
    snprintf(s, sizeof s, "smnet:%u", (unsigned)sIds[localId]);
    return s;
}

static void ns_smnet_save_id(u8 localId, UNUSED s64 networkId) {
    if (localId == 0 || localId >= MAX_PLAYERS || sMassive) { return; }
    sIds[localId] = sIds[0];
}

static void ns_smnet_clear_id(u8 localId) {
    if (localId == 0 || localId >= MAX_PLAYERS || sMassive) { return; }
    sIds[localId] = SMNET_NO_ID;
}

static void* ns_smnet_dup_addr(u8 localIndex) {
    uint16_t* a = malloc(sizeof(uint16_t));
    if (a) *a = localIndex < MAX_PLAYERS ? sIds[localIndex] : SMNET_NO_ID;
    return a;
}

static bool ns_smnet_match_addr(void* a1, void* a2) {
    return *(uint16_t*)a1 == *(uint16_t*)a2;
}

/* ---- mods de la partida: cada uno (el host y todos los que ya entraron) reparte los mods activos
   a los que se están uniendo (modsync; el que se une los baja en ns_smnet_prejoin.c). El manifiesto
   se arma con los mods activos en el orden de activación: si dos tienen los mismos archivos, tienen
   el mismo id y se reparten entre todos. */
static ms_manifest_t sGameMan;
static char (*sGameRoot)[512] = NULL;   /* carpeta raíz de cada archivo del manifiesto */
static int gm_send(void* u, uint16_t to, const uint8_t* d, int len) { (void)u; return sNode && smnet_node_send_msg(sNode, SMNET_TO_PLAYER, to, d, len) == 0 ? 0 : -1; }
static int gm_read(void* u, int file, uint32_t off, uint8_t* buf, int len) {
    (void)u;
    if (file < 0 || file >= sGameMan.nfiles) return -1;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", sGameRoot[file], sGameMan.files[file].path);
    FILE* f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, (long)off, SEEK_SET);
    int n = (int)fread(buf, 1, (size_t)len, f);
    fclose(f);
    return n;
}
static void game_mods_build(void) {
#ifndef NS_SMNET_SIN_LUA
    if (sGameSeeder) return;
    ms_manifest_free(&sGameMan);
    free(sGameRoot); sGameRoot = NULL;
    for (int i = 0; i < gActiveMods.entryCount; i++) {
        struct Mod* mod = gActiveMods.entries[i];
        if (!mod) continue;
        /* carpeta: basePath es la del mod; archivo suelto: basePath es la carpeta de mods */
        char root[512];
        snprintf(root, sizeof root, "%s", mod->basePath);
        if (mod->isDirectory) { char* sl = strrchr(root, '/'); char* bs = strrchr(root, '\\'); if (bs > sl) sl = bs; if (sl) *sl = '\0'; }
        int before = sGameMan.nfiles;
        if (ms_manifest_add_mod(&sGameMan, root, mod->relativePath) != 0) { smlog("mods: no pude leer %s/%s para repartirlo", root, mod->relativePath); }
        char (*nr)[512] = realloc(sGameRoot, sizeof(*sGameRoot) * (size_t)(sGameMan.nfiles ? sGameMan.nfiles : 1));
        if (!nr) break;
        sGameRoot = nr;
        for (int k = before; k < sGameMan.nfiles; k++) snprintf(sGameRoot[k], sizeof sGameRoot[k], "%s", root);
    }
    ms_manifest_finish(&sGameMan);
    /* firmada por el host: el host la firma; los demás le ponen la firma del host que les llegó
       al unirse (si armaron la misma lista). Sin firma no se reparte: los que se unen la piden firmada. */
    if (sOrigHost) { host_key_load(); if (sHostKeyOk) ms_manifest_sign(&sGameMan, sHostSk, sHostPub); }
    else if (sGamePubOk && sGameSigOk && memcmp(sGameManId, sGameMan.id, 16) == 0) ms_manifest_attach_sig(&sGameMan, sGamePub, sGameSig);
    if (!sGameMan.has_sig) { smlog("mods de la partida: la lista no tiene la firma del host; no se reparte"); return; }
    ms_io_t io = { NULL, gm_send, gm_read, NULL, NULL };
    sGameSeeder = ms_seeder_new(&sGameMan, &io, 6, 2000);
    char hx[33]; ms_hex(sGameMan.id, 16, hx);
    smlog("mods de la partida: %d mods, %d archivos, %.1f MB, manifiesto %s (se reparten a los que se unen)", sGameMan.nmods, sGameMan.nfiles, sGameMan.total_bytes / 1048576.0, hx);
#endif
}
static void game_mods_free(void) {
    ms_seeder_free(sGameSeeder); sGameSeeder = NULL;
    ms_manifest_free(&sGameMan);
    free(sGameRoot); sGameRoot = NULL;
}

/* telemetría anónima (ns_smnet_telemetry_send): cada 30 s, y la primera a los 15 s de conectar */
static void telem_update(double now) {
    if (!ns_smnet_telemetry_enabled() || !sNode) return;
    smnet_node_info_t inf; smnet_node_info(sNode, &inf);
    if (sTel.prevStatus == SMNET_PEER_CONNECTED && inf.status != SMNET_PEER_CONNECTED) sTel.disc++;
    if (inf.status == SMNET_PEER_CONNECTED && sTel.connAt <= 0) { sTel.connAt = now; sTel.nextAt = now + 15000; }
    sTel.prevStatus = inf.status;
    if (sTel.connAt <= 0 || now < sTel.nextAt) return;
    sTel.nextAt = now + 30000;
    const smnet_stats_t* st = smnet_node_stats(sNode);
    const smnet_link_stats_t* ls = smnet_node_link_stats(sNode);
    /* los contadores son del nodo: si se rehízo (reentrada), arrancan de cero otra vez */
#define DELTA(cur, prev) ((cur) >= (prev) ? (cur) - (prev) : (cur))
    double dt = sTel.prevAt > 0 ? (now - sTel.prevAt) / 1000.0 : 15.0;
    if (dt < 1) dt = 1;
    ns_telem_t t; memset(&t, 0, sizeof t);
    t.flags = (sOrigHost ? 1 : 0) | (inf.coord_role == SMNET_COORD_ACTIVE ? 2 : 0) | (inf.is_sn ? 4 : 0) |
              (inf.status == SMNET_PEER_CONNECTED ? 8 : 0);
    t.players = (uint16_t)(sKnownCount + 1);
    t.slots = network_player_connected_count();
    int imps = 0; for (int i = 0; i < IMP_MAX; i++) if (sImp[i].o) imps++;
    t.impostors = (uint16_t)imps;
    t.disconnects = (uint16_t)(sTel.disc > 65535 ? 65535 : sTel.disc); sTel.disc = 0;
    uint32_t fo = DELTA(inf.failovers, sTel.prevFo), rj = DELTA(inf.rejoins, sTel.prevRj);
    t.failovers = (uint16_t)(fo > 65535 ? 65535 : fo); t.rejoins = (uint16_t)(rj > 65535 ? 65535 : rj);
    sTel.prevFo = inf.failovers; sTel.prevRj = inf.rejoins;
    uint64_t hsf = DELTA(ls->hs_failed, sTel.prevHsF); t.hs_failed = (uint16_t)(hsf > 65535 ? 65535 : hsf);
    uint64_t rs = DELTA(ls->rel_sent, sTel.prevRelS), rr = DELTA(ls->rel_retx, sTel.prevRelR);
    t.retx_pct = (uint8_t)(rs ? (rr * 100 / rs > 100 ? 100 : rr * 100 / rs) : 0);
    uint64_t rx = DELTA(st->rx_bytes, sTel.prevRx), tx = DELTA(st->tx_bytes, sTel.prevTx);
    double rk = rx * 8.0 / 1000.0 / dt, tk = tx * 8.0 / 1000.0 / dt;
    t.rx_kbps = (uint16_t)(rk > 65535 ? 65535 : rk); t.tx_kbps = (uint16_t)(tk > 65535 ? 65535 : tk);
    sTel.prevHsF = ls->hs_failed; sTel.prevRelS = ls->rel_sent; sTel.prevRelR = ls->rel_retx;
    sTel.prevRx = st->rx_bytes; sTel.prevTx = st->tx_bytes; sTel.prevAt = now;
#undef DELTA
    t.sn_hosted = (uint16_t)(inf.is_sn && inf.hosted > 0 ? inf.hosted : 0);
    t.uptime_s = (uint32_t)((now - sTel.connAt) / 1000.0);
    ns_smnet_telemetry_send(sFd, &t);
}

static void ns_smnet_update(void) {
    if (gNetworkType == NT_NONE || !sNode) { return; }
    uint8_t buf[2048];
    smnet_addr_t from;
    int len;
    double now = smnet_now_ms();
    /* fin de la partida (lo pidió el host, o lo pide uno siendo el host): fuera de los callbacks de smnet */
    if (sMassive && ns_smnet_mod_kicked()) {
        int why = ns_smnet_mod_kicked();
        extern void djui_panel_pause_quit_yes(struct DjuiBase* caller);
        djui_panel_pause_quit_yes(NULL);
        djui_popup_create(why == 2 ? "\\#ffa0a0\\Te banearon de la partida." : "\\#ffa0a0\\Te echaron de la partida.", 1);
        return;
    }
    if (sEndRx || (sEndAt > 0 && now >= sEndAt)) {
        bool rx = sEndRx;
        sEndRx = false; sEndAt = 0;
        extern void djui_panel_pause_quit_yes(struct DjuiBase* caller);
        djui_panel_pause_quit_yes(NULL);
        if (rx) djui_popup_create("\\#ffa0a0\\El host finalizó la partida.", 1);
        return;
    }
    while ((len = smnet_udp_recv(sFd, &from, buf, sizeof buf)) >= 0) {
        /* el encuentro de coopnet (registro del host, avisos de perforación, sondas de NAT) */
        if (ns_rv_is_msg(buf, len)) { ns_smnet_lobby_rv_rx(sFd, &from, buf, len, now); continue; }
        smnet_node_receive(sNode, &from, buf, len, now);
        if (sOrigHost) peers_note(&from, now);
    }
    smnet_node_update(sNode, now);
    if (sNumPending) flush_pending();
    if (sMassive) {
        rejoin_update(now);
        telem_update(now);
        ns_smnet_mod_update(now);
        /* cerrada: en la lista de coopnet figura llena (el encuentro no deja entrar a nadie nuevo) */
        ns_smnet_lobby_update(sFd, ns_smnet_mod_closed() ? (int)configAmountOfPlayers : sKnownCount + 1, now);   /* host listado en coopnet */
        if (sMassiveJoin && sJoinState != 2) ns_smnet_lobby_join_ping(sFd, now);   /* que el host perfore hacia este socket */
        if (sOrigHost) {
            smnet_node_info_t inf; smnet_node_info(sNode, &inf);
            if (inf.status == SMNET_PEER_CONNECTED && sRejoinN == 0) {
                if (sHostConnAt <= 0) sHostConnAt = now;
                if (sClaimedAs != inf.id) { sClaimedAs = inf.id; host_claim_send(SMNET_TO_ALL, 0); sClaimAt = now; sClaimN = 0; smlog("host: anunciado como smnet:%u", inf.id); }
                else {
                    /* otra vez a los 2, 5, 10 y 20 s del cambio de id, y después cada 30 s: tras un
                       cambio de coordinador los demás pueden descartar el primero (id que todavía no conocen) */
                    static const double kAgain[] = { 2000, 5000, 10000, 20000 };
                    double wait = sClaimN < 4 ? kAgain[sClaimN] - (sClaimN ? kAgain[sClaimN - 1] : 0) : 30000;
                    if (now - sClaimAt > wait) { host_claim_send(SMNET_TO_ALL, 0); sClaimAt = now; sClaimN++; }
                }
                if (now - sStateSaved > 10000) { sStateSaved = now; game_state_save(now); }
            } else if (inf.status != SMNET_PEER_CONNECTED) { sHostConnAt = 0; sClaimedAs = SMNET_NO_ID; }
        }
        /* los 12 B de estado salen siempre (para el interés y los slots de los demás); el blob
           solo cuando sm64coopdx arma un PLAYER */
        struct MarioState* m = &gMarioStates[0];
        if (m->marioObj != NULL) {
            extern s16 gCurrActStarNum, gCurrLevelNum, gCurrAreaIndex;
            smnet_state_t st;
            memset(&st, 0, sizeof st);
            st.x = m->pos[0]; st.y = m->pos[1]; st.z = m->pos[2];
            st.yaw = m->faceAngle[1]; st.fwd_vel = m->forwardVel; st.vel_y = m->vel[1];
            state_fill_pose(m, &st);
            smnet_node_set_local(sNode, area_key((unsigned)gCurrLevelNum, (unsigned)gCurrAreaIndex, (unsigned)gCurrActStarNum), &st, NULL, 0);
        }
        slots_update(now);
        impostors_update(now);
        lua_relay_flush();
        if (!sGameSeeder && now - sGameModsTried > 30000) { sGameModsTried = now; game_mods_build(); }
        if (sGameSeeder) ms_seeder_update(sGameSeeder, now);
        join_status_update(now);
        world_area_update(now);
        lua_global_update();
        /* el modelo propio cambió (Character Select y parecidos): se vuelve a anunciar */
        { static double last = 0; static uint16_t lastExt = 0xFFFF;
          extern s16 gCurrActStarNum, gCurrLevelNum, gCurrAreaIndex;
          static int lastWhere = -1;
          int where = (gCurrLevelNum << 8) | ((gCurrAreaIndex & 0xF) << 4) | (gCurrActStarNum & 0xF);
          if (now - last >= 1000) { last = now; uint16_t e = own_model_ext(); if (e != lastExt || where != lastWhere) { lastExt = e; lastWhere = where; publish_info(); } } }
        /* sm64coopdx solo arma el PLAYER si hay algún otro jugador conectado; en este modo hay
           que publicarlo igual estando solo (para que el que llega nos vea enseguida y para
           grabar la plantilla de los bots) */
        static double sLastAlone = 0;
        if (!network_player_any_connected() && now - sLastAlone >= 100) {
            sLastAlone = now;
            network_send_player(0);
        }
    }
    if (now - sLastStatus > 1000) { sLastStatus = now; log_status("estado"); }
    /* el cliente: el "servidor" de sm64coopdx es el coordinador (el host) */
    if (gNetworkType == NT_CLIENT && !sMassive) {
        smnet_node_info_t inf;
        smnet_node_info(sNode, &inf);
        if (inf.status == SMNET_PEER_CONNECTED && sIds[0] == SMNET_NO_ID) sIds[0] = inf.coord_id;
    }
}

static int ns_smnet_send(u8 localIndex, void* address, u8* data, u16 dataLength) {
    if (!sNode) { return -1; }
    /* modo masivo: los "clientes" son slots virtuales; el PLAYER propio sale por
       ns_smnet_publish_player_packet y el resto, por ahora, no sale */
    if (sMassive) { (void)localIndex; (void)address; (void)data; (void)dataLength; return 0; }
    if (localIndex != 0) {
        if (gNetworkType == NT_SERVER && gNetworkPlayers[localIndex].type != NPT_CLIENT) { return -1; }
        if (gNetworkType == NT_CLIENT && gNetworkPlayers[localIndex].type != NPT_SERVER) { return -1; }
    }
    uint8_t gi;
    uint16_t ak;
    if (player_pkt_info(data, dataLength, &gi, &ak)) {
        /* el propio va al flujo de estado (el host lo llama una vez por cliente: da igual);
           los ajenos son reenvíos del host, que ya les llegaron por smnet */
        if (gNetworkPlayerLocal != NULL && gi == gNetworkPlayerLocal->globalIndex) publish_player(ak, data, dataLength);
        return 0;
    }
    uint16_t to = (localIndex == 0 && address != NULL) ? *(uint16_t*)address : sIds[localIndex];
    smnet_node_info_t inf;
    smnet_node_info(sNode, &inf);
    if (inf.status != SMNET_PEER_CONNECTED) {
        /* todavía entrando: se guarda (el JOIN_REQUEST sale antes de que smnet termine de entrar) */
        if (sNumPending == PENDING_MAX) { return -1; }
        uint8_t* cp = malloc(dataLength);
        if (!cp) { return -1; }
        memcpy(cp, data, dataLength);
        sPending[sNumPending].to = (gNetworkType == NT_CLIENT) ? SMNET_NO_ID : to;
        sPending[sNumPending].len = dataLength;
        sPending[sNumPending].data = cp;
        sNumPending++;
        smlog("todavía sin entrar a smnet: se guarda el paquete (%s, %u B)", pkt_name(data, dataLength), dataLength);
        return 0;
    }
    if (to == SMNET_NO_ID && gNetworkType == NT_CLIENT) { to = inf.coord_id; }
    if (to == SMNET_NO_ID) { return -1; }
    smlog("enviando a smnet:%u: %u B, %s, índice %d", to, dataLength, pkt_name(data, dataLength), localIndex);
    if (send_frags(to, data, dataLength) != 0) {
        smlog("  falló el envío");
        LOG_ERROR("smnet: no se pudo mandar (índice %d, tipo %d, %d B)", localIndex, data[0], dataLength);
        return -1;
    }
    return 0;
}

static void ns_smnet_get_lobby_id(char* destination, u32 destLength) { snprintf(destination, destLength, "%s", ""); }
static void ns_smnet_get_lobby_secret(char* destination, u32 destLength) { snprintf(destination, destLength, "%s", ""); }

static void ns_smnet_shutdown(bool reconnecting) {
    log_status("al apagar");
    if (sOrigHost && !sEnding) game_state_save(smnet_now_ms());   /* salió sin finalizar: puede volver */
    bool wasJoin = sMassive && !sOrigHost && sMassiveJoin;
    if (sNode) {
        smnet_node_leave(sNode, smnet_now_ms());
        smnet_node_free(sNode);
        sNode = NULL;
    }
    if (sFd >= 0) { smnet_udp_close(sFd); sFd = -1; }
    for (int i = 0; i < sNumPending; i++) free(sPending[i].data);
    sNumPending = 0;
    for (int i = 0; i < SMNET_MAX_PLAYERS; i++) { free(sRe[i].data); sRe[i].data = NULL; sRe[i].len = 0; }
    for (int i = 0; i < MAX_PLAYERS; i++) sIds[i] = SMNET_NO_ID;
    if (sMassive) imp_forget_all(true);
    game_mods_free();
    sMassive = sMassiveJoin = false;
    sOrigHost = false; sHostId = SMNET_NO_ID; sRejoinN = 0; sEndAt = 0; sEndRx = sEnding = false;
    sGamePubOk = sGameSigOk = false;
    memset(sSlotOf, 0, sizeof sSlotOf); memset(sSlotMade, 0, sizeof sSlotMade);
    if (wasJoin && !reconnecting) ns_smnet_mods_restore_own();   /* vuelven los mods que tenía el usuario */
    if (!reconnecting) ns_smnet_lobby_stop();                     /* el lobby de coopnet, si había */
    LOG_INFO("smnet: apagado");
    smlog("apagado");
    if (sLog) { fclose(sLog); sLog = NULL; }
}

struct NetworkSystem gNetworkSystemSmnet = {
    .initialize       = ns_smnet_initialize,
    .get_id           = ns_smnet_get_id,
    .get_id_str       = ns_smnet_get_id_str,
    .save_id          = ns_smnet_save_id,
    .clear_id         = ns_smnet_clear_id,
    .dup_addr         = ns_smnet_dup_addr,
    .match_addr       = ns_smnet_match_addr,
    .update           = ns_smnet_update,
    .send             = ns_smnet_send,
    .get_lobby_id     = ns_smnet_get_lobby_id,
    .get_lobby_secret = ns_smnet_get_lobby_secret,
    .shutdown         = ns_smnet_shutdown,
    .requireServerBroadcast = true,
    .name             = "smnet",
};
#endif
