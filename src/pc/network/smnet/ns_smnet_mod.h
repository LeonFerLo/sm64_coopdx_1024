#ifndef NS_SMNET_MOD_H
#define NS_SMNET_MOD_H
/* Moderación de las partidas masivas (ns_smnet_mod.c).
 *
 * La autoridad es el host de la partida (y los moderadores que él nombra, que le piden a él). El
 * host guarda las listas y se las manda a todos cada 5 s y cuando cambian (APP_MODLIST):
 *   - echados y baneados: todos los descartan (no se dibujan, su chat y sus mensajes no se usan),
 *     el echado recibe APP_KICKED y sale; el coordinador de smnet lo saca de la tabla y bloquea su
 *     identidad (5 min si lo echaron; toda la partida o para siempre si lo banearon). Los baneos
 *     "para siempre" el host los guarda en smnet-baneados.txt.
 *   - silenciados: todos descartan su chat.
 *   - partida cerrada: el coordinador solo deja volver a los que ya estaban, y el host la informa
 *     llena en la lista de coopnet.
 * Quien sea coordinador aplica las listas a smnet (también si toma el control otro nodo). */
#ifdef SMNET
#include <stdbool.h>
#include <stdint.h>

#define APP_MODLIST 0x8D    /* host → todos: listas (ver ns_smnet_mod.c) */
#define APP_KICKED  0x8E    /* host → el echado: [motivo u8: 1 echado, 2 baneado] */
#define APP_MODREQ  0x8F    /* moderador → host, host ↔ coordinador: [op u8][id u16]... */

enum { NS_MOD_KICK = 1, NS_MOD_BAN, NS_MOD_PERMBAN, NS_MOD_MUTE, NS_MOD_UNMUTE, NS_MOD_MODERATOR, NS_MOD_UNMODERATOR,
       NS_MOD_CLOSE, NS_MOD_OPEN, NS_MOD_UNBAN_ALL };   /* los tres últimos: de un administrador al host */

void ns_smnet_mod_reset(bool isHost);
void ns_smnet_mod_update(double now);
void ns_smnet_mod_rx(uint16_t from, const uint8_t* d, int len, double now);
bool ns_smnet_mod_hidden(uint16_t id);
bool ns_smnet_mod_muted(uint16_t id);
bool ns_smnet_mod_closed(void);
bool ns_smnet_mod_is_moderator(uint16_t id);
bool ns_smnet_mod_can_moderate(void);
/* administrador del host dedicado (COOPDX_SMNET_ADMINS en el host): tiene los permisos del host */
bool ns_smnet_mod_is_admin(uint16_t id);
bool ns_smnet_mod_is_boss(void);   /* el host o un administrador */
/* motivo si a uno lo echaron (1) o banearon (2) y hay que salir; 0 si no */
int  ns_smnet_mod_kicked(void);

/* chat: 1 si era un comando de moderación (ya contestado), 0 si no */
int  ns_smnet_mod_command(const char* cmd);

/* para el panel de moderación */
int  ns_smnet_mod_search(const char* text, uint16_t* out, int cap);
void ns_smnet_mod_name(uint16_t id, char* out, int cap);
bool ns_smnet_mod_action(uint16_t id, int op);     /* false si no se pudo (sin permiso, uno mismo...) */
void ns_smnet_mod_set_closed(bool closed);
void ns_smnet_mod_unban_all(void);

/* lo que el módulo necesita de ns_smnet.c */
struct smnet_node;
struct smnet_node* ns_smnet_node_ptr(void);
uint16_t ns_smnet_host_smnet_id(void);           /* el host verificado (SMNET_NO_ID si es uno mismo o no está) */
bool ns_smnet_is_orig_host_now(void);
uint16_t ns_smnet_self_smnet_id(void);
bool ns_smnet_known_player(uint16_t id);
void ns_smnet_player_name(uint16_t id, char* out, int cap);
#endif
#endif
