/* Ayudante de UDP crudo para las herramientas (bots). El juego usa su propio socket.
 * Direcciones en forma canónica de 6 bytes (IPv4 + puerto). Linux, macOS y Windows. */
#ifndef SMNET_UDP_H
#define SMNET_UDP_H

#include "smnet.h"

#ifdef __cplusplus
extern "C" {
#endif

int  smnet_udp_open(uint16_t port);                           /* socket no bloqueante; port 0 = efímero */
int  smnet_udp_open_on(int loopback_only, uint16_t port);     /* loopback_only: solo 127.0.0.1 */
int  smnet_udp_send(int fd, const smnet_addr_t* to, const uint8_t* data, int len);
int  smnet_udp_recv(int fd, smnet_addr_t* from, uint8_t* buf, int cap);   /* -1 si no hay nada */
int  smnet_addr_parse(const char* host, uint16_t port, smnet_addr_t* out);
void smnet_udp_close(int fd);
int  smnet_udp_local_addr(int fd, const char* host, smnet_addr_t* out);  /* host:puerto propio */
double smnet_now_ms(void);                                     /* reloj monotónico */
double smnet_wall_ms(void);                                    /* reloj de pared (entre procesos/máquinas) */
uint16_t smnet_addr_port(const smnet_addr_t* a);
int  smnet_random(uint8_t* out, int n);                        /* entropía del sistema (semilla) */
int  smnet_udp_lan_ip(char out[16]);                           /* IP local de salida ("127.0.0.1" si no hay) */

#ifdef __cplusplus
}
#endif
#endif
