#include <iostream>
#include <cstdio>
#include "server.hpp"
#include "metrics.hpp"
#include "extra/server_extra.hpp"
#include "sha2.hpp"
#include "rendezvous.hpp"
#include <cstdlib>

#define PORT 34197
#define EXIT_FAILURE 1

int main(int argc, char *argv[]) {
    Metrics metrics;

    gServer = new Server();

    gCoopNetCallbacks.DestIdFunction = sha224_u64;
    server_extra_init();

    /* COOPNET_PUERTO (34197) y COOPNET_ENCUENTRO (34198; usa también el siguiente) para probar
       varios en la misma máquina */
    const char* ep = getenv("COOPNET_PUERTO");
    uint32_t port = (ep && atoi(ep) > 0) ? (uint32_t)atoi(ep) : PORT;
    const char* er = getenv("COOPNET_ENCUENTRO");
    uint16_t rport = (er && atoi(er) > 0) ? (uint16_t)atoi(er) : (uint16_t)(PORT + 1);

    if (!gServer->Begin(port)) {
        printf("coopnet: no se pudo abrir el puerto TCP %u\n", (unsigned)port);
        exit(EXIT_FAILURE);
    }
    printf("coopnet: escuchando en TCP %u\n", (unsigned)port); fflush(stdout);
    /* encuentro UDP de las partidas masivas (smnet): si no abre, las clásicas siguen andando */
    rendezvous_begin(rport, (uint16_t)(rport + 1));

    while (true) {
        metrics.Update(gServer->LobbyCount(), gServer->PlayerCount());
        server_extra_update();
        std::this_thread::sleep_for(std::chrono::milliseconds(20 * 1000));
    }

    return 0;
}
