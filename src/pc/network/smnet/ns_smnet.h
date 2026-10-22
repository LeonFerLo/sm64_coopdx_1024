#ifndef NS_SMNET_H
#define NS_SMNET_H
#ifdef SMNET
#include "pc/network/network.h"
extern struct NetworkSystem gNetworkSystemSmnet;
/* Compilado con SMNET=1, la conexión directa usa smnet salvo que COOPDX_SMNET=0. */
bool ns_smnet_wanted(void);
/* Modo masivo (más de 16): COOPDX_SMNET_MASIVO=1 en el host, COOPDX_SMNET_UNIRSE=ip[:puerto] en
   los demás; todos usan "Host" en el menú. Cada uno simula su mundo y ve a los 15 más cercanos de
   su área como Marios completos (slots). */
bool ns_smnet_massive(void);
/* Host abre partida masiva salvo COOPDX_SMNET_MASIVO=0 */
bool ns_smnet_massive_default(void);
/* Join por IP en modo masivo: guarda el destino; después se arranca como Host */
void ns_smnet_request_massive_join(const char* ip, unsigned port);
/* Join masivo, primer paso: baja y activa los mods de la partida (ns_smnet_prejoin.c) y después
   sigue con ns_smnet_request_massive_join + Host. false si no pudo empezar. */
struct DjuiBase;
bool ns_smnet_prejoin_begin(struct DjuiBase* caller, const char* ip, unsigned port);
void ns_smnet_prejoin_tick(void);
struct Packet;
/* el PLAYER propio en modo masivo: va al flujo de estado sin pasar por network_send */
void ns_smnet_publish_player_packet(struct Packet* p);
/* packet_player.c: posición de los campos en PacketPlayerData (plantilla de los bots) */
void ns_smnet_player_offsets(u16* pos, u16* faceAngle, u16* intendedYaw, u16* size);
/* chat global del modo masivo (packet_chat.c) */
void ns_smnet_send_chat(const char* message);
/* funciones de Lua para los mods (smlua_functions.c): ver ns_smnet.c, "API de Lua" */
/* impostores con el modelo de Mario (mario_misc.c): índice k si el objeto es uno (o -1), y sus
   datos de dibujo */
struct Object; struct PlayerPalette; struct MarioState;
s32 ns_smnet_impostor_index(struct Object* o);
const struct PlayerPalette* ns_smnet_impostor_palette(s32 k);
u8 ns_smnet_impostor_model(s32 k);
struct MarioState* ns_smnet_impostor_mario_state(s32 k);
struct lua_State;
void ns_smnet_lua_bind(struct lua_State* L);
struct Packet;
struct LSTNetworkType;
/* modo masivo: paquetes de Lua de los mods por smnet (true = ya se encargó) */
bool ns_smnet_relay_packet(struct Packet* p, u8 toLocalIndex);
/* modo masivo: corrige las claves de un campo de gPlayerSyncTable que llegó (false = descartar) */
bool ns_smnet_lua_sync_keys(u8 fromLocalIndex, u16 n, struct LSTNetworkType* keys);
/* nombre, modelo o paleta cambiados en el menú: se vuelven a anunciar */
void ns_smnet_settings_changed(void);
/* Ajustes → Partida masiva cambiados (impostores; la subida y la telemetría valen desde la próxima partida) */
void ns_smnet_options_apply(void);
/* índice local para un índice global que llegó dentro de un paquete de Lua (-1 = el normal) */
s32 ns_smnet_remap_global(u8 globalIndex);
/* mundo compartido: si el de este slot simula objetos (los bots no) y el id de smnet propio */
bool ns_smnet_slot_simulates(u8 localIndex);
s32 ns_smnet_local_smnet_id(void);
/* procesando un paquete del mundo que llegó de otro jugador (modo masivo) */
bool ns_smnet_rx_from_other(void);
/* procesando gGlobalSyncTable que mandó el coordinador (se acepta siempre) */
bool ns_smnet_rx_global_authority(void);
/* lista global de jugadores (TAB en modo masivo) */
struct NsSmnetListEntry { char name[40]; u8 model; u8 rgb[3]; s16 level; u8 area, act; bool hasLocation, self, slot; s8 localIndex; };
int ns_smnet_list_count(void);
/* el primer byte de los datos del nodo de un Join que todavía baja mods (no es un jugador) */
#define NS_SMNET_PREJOIN_MARK 0xFE
/* el host de la partida (clave propia en smnet-host.key): la clave y la firma de la lista de mods
   que llegaron al unirse (ns_smnet_prejoin.c, antes de arrancar) */
void ns_smnet_set_game_host(const uint8_t pub[32], const uint8_t sig[64], const uint8_t manId[16]);
/* pausa: este es el host de la partida masiva / se unió a la de otro; el host la finaliza para todos */
bool ns_smnet_is_game_host(void);
bool ns_smnet_joined_game(void);
void ns_smnet_end_game(void);
/* propiedad de objetos: otro jugador (sin slot acá) que simula la misma área está más cerca */
bool ns_smnet_other_owner_closer(f32 x, f32 y, f32 z, f32 myDist);
/* los mods propios del usuario: se guardan al unirse a una partida (que activa los de ella) y
   vuelven al salir o al arrancar el juego si quedaron guardados */
void ns_smnet_mods_save_own(void);
void ns_smnet_mods_restore_own(void);
bool ns_smnet_list_get(int k, struct NsSmnetListEntry* e);
#endif
#endif
