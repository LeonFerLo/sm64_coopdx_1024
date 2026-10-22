/* Prueba de las partidas masivas del coopnet modificado (lobbies y encuentro UDP), sin el juego.
 *
 *   prueba_masivo host    PUERTO_TCP PUERTO_UDP [máx] [contraseña]   crea una partida y se registra
 *   prueba_masivo jugador PUERTO_TCP PUERTO_UDP [contraseña]         la busca en la lista, mide su NAT y entra
 *
 * Compilar (desde coopnet/, después de make lib):
 *   g++ -std=c++11 -Icommon -Ilib/include dev/prueba_masivo.cpp bin/libcoopnet.a -Llib/linux -l:libjuice.a -pthread -o bin/prueba_masivo */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include "libcoopnet.h"

static bool sConnected = false, sListDone = false;
static uint64_t sLobby = 0, sFoundLobby = 0;
static uint8_t sSecret[16];
static uint16_t sPortA = 0, sPortB = 0;
static int sLastError = -1;
static int sFoundMax = 0, sFoundCount = 0;

static void OnConnected(uint64_t id) { printf("conectado al servidor (usuario %llu)\n", (unsigned long long)id); sConnected = true; }
static void OnDisconnected(bool i) { printf("desconectado (%d)\n", i); }
static void OnLobbyCreated(uint64_t id, const char*, const char*, const char*, const char* mode, uint16_t mx) {
    printf("partida creada: id %llu, modo %s, máximo %u\n", (unsigned long long)id, mode, mx);
    sLobby = id;
}
static void OnMassive(uint64_t id, const uint8_t sec[16], uint16_t a, uint16_t b) {
    printf("es masiva: clave recibida, encuentro en %u/%u\n", a, b);
    memcpy(sSecret, sec, 16); sPortA = a; sPortB = b;
}
static void OnListGot(uint64_t id, uint64_t, uint16_t con, uint16_t mx, const char*, const char*, const char* host, const char* mode, const char*) {
    printf("  lista: %llu  %s  %s  %u/%u%s\n", (unsigned long long)id, host, mode, con, mx, mx > 16 ? "  (masiva)" : "");
    if (mx > 16) { sFoundLobby = id; sFoundMax = mx; sFoundCount = con; }
}
static void OnListFinish(void) { sListDone = true; }
static void OnError(enum MPacketErrorNumber e, uint64_t) { printf("error de coopnet: %d%s\n", (int)e, e == MERR_LOBBY_MASSIVE ? " (MERR_LOBBY_MASSIVE: se entra por el encuentro)" : ""); sLastError = (int)e; }

static int udp_open(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = 0; a.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(s, (struct sockaddr*)&a, sizeof a);
    fcntl(s, F_SETFL, O_NONBLOCK);
    return s;
}
static struct sockaddr_in addr(uint16_t port) {
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = inet_addr("127.0.0.1");
    return a;
}
static void hdr(uint8_t* b, uint8_t t) { memcpy(b, "SMRV", 4); b[4] = t; }
static int recv_wait(int s, uint8_t* b, int cap, int ms, struct sockaddr_in* from) {
    for (int i = 0; i < ms / 10; i++) {
        socklen_t fl = sizeof *from;
        int n = (int)recvfrom(s, b, cap, 0, (struct sockaddr*)from, &fl);
        if (n > 0) return n;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return -1;
}
static void pump(int ms) { for (int i = 0; i < ms / 10; i++) { coopnet_update(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); } }
static const char* ipstr(uint32_t ip) { struct in_addr a; a.s_addr = ip; return inet_ntoa(a); }

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "uso: host|jugador PUERTO_TCP PUERTO_UDP ...\n"); return 2; }
    bool host = !strcmp(argv[1], "host");
    uint32_t tcp = (uint32_t)atoi(argv[2]);
    uint16_t udpA = (uint16_t)atoi(argv[3]);
    gCoopNetCallbacks.OnConnected = OnConnected;
    gCoopNetCallbacks.OnDisconnected = OnDisconnected;
    gCoopNetCallbacks.OnLobbyCreated = OnLobbyCreated;
    gCoopNetCallbacks.OnMassiveLobby = OnMassive;
    gCoopNetCallbacks.OnLobbyListGot = OnListGot;
    gCoopNetCallbacks.OnLobbyListFinish = OnListFinish;
    gCoopNetCallbacks.OnError = OnError;
    if (coopnet_begin("127.0.0.1", tcp, host ? "host-prueba" : "jugador-prueba", (uint64_t)rand()) != COOPNET_OK) { printf("no conecta\n"); return 1; }
    for (int i = 0; i < 300 && !sConnected; i++) pump(10);
    if (!sConnected) { printf("no conectó al servidor\n"); return 1; }
    int s = udp_open();
    uint8_t b[256]; struct sockaddr_in from;
    if (host) {
        int mx = argc > 4 ? atoi(argv[4]) : 512;
        const char* pw = argc > 5 ? argv[5] : "";
        coopnet_lobby_create("sm64coopdx", "v1.5.1", "host-prueba", "masivo", (uint16_t)mx, pw, "partida de prueba");
        for (int i = 0; i < 300 && !sPortA; i++) pump(10);
        if (!sPortA) { printf("no llegó la clave (¿no es masiva?)\n"); pump(2000); return 0; }
        struct sockaddr_in A = addr(sPortA);
        for (int t = 0; t < 60; t++) {
            if (t % 4 == 0) {
                hdr(b, 3); memcpy(b + 5, &sLobby, 8); memcpy(b + 13, sSecret, 16); uint16_t c = 37; memcpy(b + 29, &c, 2);
                sendto(s, b, 31, 0, (struct sockaddr*)&A, sizeof A);
            }
            int n;
            while ((n = recv_wait(s, b, sizeof b, 10, &from)) > 0) {
                if (n >= 12 && b[4] == 4) { uint32_t ip; uint16_t p; memcpy(&ip, b + 6, 4); memcpy(&p, b + 10, 2); if (t == 0) printf("registro: estado %d, dirección pública %s:%u\n", b[5], ipstr(ip), ntohs(p)); }
                else if (n >= 11 && b[4] == 7) { uint32_t ip; uint16_t p; memcpy(&ip, b + 5, 4); memcpy(&p, b + 9, 2); printf("PERFORAR hacia %s:%u: se manda un paquete\n", ipstr(ip), ntohs(p));
                    struct sockaddr_in j; memset(&j, 0, sizeof j); j.sin_family = AF_INET; j.sin_addr.s_addr = ip; j.sin_port = p;
                    sendto(s, "hola-del-host", 13, 0, (struct sockaddr*)&j, sizeof j); }
                else if (n == 15 && !memcmp(b, "hola-del-jugad", 14)) printf("llegó el saludo del jugador directo al socket del host\n");
            }
            pump(240);
        }
        return 0;
    }
    const char* pw = argc > 4 ? argv[4] : "";
    coopnet_lobby_list_get("sm64coopdx", pw);
    for (int i = 0; i < 300 && !sListDone; i++) pump(10);
    if (!sFoundLobby) { printf("no hay partidas masivas en la lista\n"); return 1; }
    coopnet_lobby_join(sFoundLobby, pw);
    pump(500);
    /* tipo de NAT: una sonda a cada puerto desde el mismo socket */
    struct sockaddr_in A = addr(udpA), B = addr((uint16_t)(udpA + 1));
    uint16_t pubPort[2] = { 0, 0 };
    for (int k = 0; k < 2; k++) {
        hdr(b, 1); uint32_t nonce = 1000 + k; memcpy(b + 5, &nonce, 4);
        sendto(s, b, 9, 0, (struct sockaddr*)(k ? &B : &A), sizeof A);
        int n = recv_wait(s, b, sizeof b, 1000, &from);
        if (n >= 16 && b[4] == 2) { memcpy(&pubPort[k], b + 13, 2); }
    }
    struct sockaddr_in me; socklen_t ml = sizeof me; getsockname(s, (struct sockaddr*)&me, &ml);
    int nat = (!pubPort[0] || !pubPort[1]) ? 3 : pubPort[0] != pubPort[1] ? 2 : pubPort[0] == me.sin_port ? 0 : 1;
    const char* natName[] = { "sin NAT", "de cono", "simétrico", "sin respuesta" };
    printf("NAT: %s (puerto local %u, público %u / %u)\n", natName[nat], ntohs(me.sin_port), ntohs(pubPort[0]), ntohs(pubPort[1]));
    hdr(b, 8); b[5] = (uint8_t)nat; sendto(s, b, 6, 0, (struct sockaddr*)&A, sizeof A);
    /* entrar */
    hdr(b, 5); memcpy(b + 5, &sFoundLobby, 8); uint32_t nonce = 77; memcpy(b + 13, &nonce, 4);
    int pl = (int)strlen(pw); b[17] = (uint8_t)pl; memcpy(b + 18, pw, (size_t)pl);
    sendto(s, b, 18 + pl, 0, (struct sockaddr*)&A, sizeof A);
    int n = recv_wait(s, b, sizeof b, 1000, &from);
    if (n < 16 || b[4] != 6) { printf("sin respuesta para entrar\n"); return 1; }
    uint32_t hip; uint16_t hp; memcpy(&hip, b + 10, 4); memcpy(&hp, b + 14, 2);
    printf("entrar: estado %d, host en %s:%u\n", b[9], ipstr(hip), ntohs(hp));
    if (b[9] != 0) return 1;
    struct sockaddr_in H; memset(&H, 0, sizeof H); H.sin_family = AF_INET; H.sin_addr.s_addr = hip; H.sin_port = hp;
    sendto(s, "hola-del-jugador", 15, 0, (struct sockaddr*)&H, sizeof H);
    n = recv_wait(s, b, sizeof b, 2000, &from);
    if (n > 0 && from.sin_addr.s_addr == hip && from.sin_port == hp) printf("llegó la perforación del host (%d B desde %s:%u): ya se pueden hablar\n", n, ipstr(hip), ntohs(hp));
    else printf("no llegó la perforación del host\n");
    return 0;
}
