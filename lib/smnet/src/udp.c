/* Ayudante UDP (Linux, macOS, Windows) para las herramientas y el juego. Las direcciones van en
 * forma canónica de 6 bytes (IPv4 + puerto, big-endian): iguales byte a byte vengan de recvfrom o
 * de parse, en cualquier sistema (macOS agrega sin_len a sockaddr_in). */
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <bcrypt.h>
#  include <string.h>
#  include <time.h>
   typedef int socklen_t;
#  define close_fd(fd) closesocket((SOCKET)(fd))
#else
#  if !defined(__APPLE__)
#    define _POSIX_C_SOURCE 200809L
#  endif
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <stdio.h>
#  include <string.h>
#  include <sys/socket.h>
#  include <time.h>
#  include <unistd.h>
#  define close_fd(fd) close(fd)
#endif
#include "smnet_udp.h"

static void to_canon(const struct sockaddr_in* a, smnet_addr_t* out) {
    memset(out, 0, sizeof *out);
    out->len = 6;
    memcpy(out->data, &a->sin_addr.s_addr, 4);
    memcpy(out->data + 4, &a->sin_port, 2);
}

static void from_canon(const smnet_addr_t* in, struct sockaddr_in* a) {
    memset(a, 0, sizeof *a);
#ifdef __APPLE__
    a->sin_len = sizeof *a;
#endif
    a->sin_family = AF_INET;
    memcpy(&a->sin_addr.s_addr, in->data, 4);
    memcpy(&a->sin_port, in->data + 4, 2);
}

int smnet_udp_open(uint16_t port) { return smnet_udp_open_on(0, port); }

int smnet_udp_open_on(int loopback_only, uint16_t port) {
#if defined(_WIN32)
    static int wsa_ok = 0;
    if (!wsa_ok) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return -1; wsa_ok = 1; }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return -1;
    int fd = (int)s;
#else
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
#endif
    int big = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&big, sizeof big);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&big, sizeof big);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
#ifdef __APPLE__
    a.sin_len = sizeof a;
#endif
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    a.sin_port = htons(port);
#if defined(_WIN32)
    if (bind(fd, (struct sockaddr*)&a, sizeof a) != 0) { close_fd(fd); return -1; }
    u_long nb = 1;
    ioctlsocket((SOCKET)fd, FIONBIO, &nb);
    /* sin esto, un ICMP "puerto inalcanzable" hace fallar el próximo recvfrom con WSAECONNRESET */
    BOOL no = FALSE; DWORD ret = 0;
    WSAIoctl((SOCKET)fd, _WSAIOW(IOC_VENDOR, 12), &no, sizeof no, NULL, 0, &ret, NULL, NULL);
#else
    if (bind(fd, (struct sockaddr*)&a, sizeof a) != 0) { int e = errno; close_fd(fd); errno = e; return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
    return fd;
}

int smnet_udp_send(int fd, const smnet_addr_t* to, const uint8_t* data, int len) {
    if (to->len != 6) return -1;
    struct sockaddr_in a;
    from_canon(to, &a);
    return (int)sendto(fd, (const char*)data, len, 0, (const struct sockaddr*)&a, sizeof a);
}

int smnet_udp_recv(int fd, smnet_addr_t* from, uint8_t* buf, int cap) {
    struct sockaddr_in a;
    socklen_t sl = sizeof a;
    int n = (int)recvfrom(fd, (char*)buf, cap, 0, (struct sockaddr*)&a, &sl);
    if (n < 0) return -1;
    to_canon(&a, from);
    return (int)n;
}

int smnet_addr_parse(const char* host, uint16_t port, smnet_addr_t* out) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return -1;
        a.sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    to_canon(&a, out);
    return 0;
}

uint16_t smnet_addr_port(const smnet_addr_t* a) { return a->len == 6 ? (uint16_t)((a->data[4] << 8) | a->data[5]) : 0; }

void smnet_udp_close(int fd) { close_fd(fd); }

int smnet_udp_local_addr(int fd, const char* host, smnet_addr_t* out) {
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    if (getsockname(fd, (struct sockaddr*)&a, &l) != 0) return -1;
    return smnet_addr_parse(host, ntohs(a.sin_port), out);
}

#if defined(_WIN32)
double smnet_now_ms(void) {
    static LARGE_INTEGER f = { 0 };
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
double smnet_wall_ms(void) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (double)(u.QuadPart - 116444736000000000ULL) / 10000.0;
}
int smnet_random(uint8_t* out, int n) {
    return BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
}
#else
double smnet_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
double smnet_wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
int smnet_random(uint8_t* out, int n) {
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t got = fread(out, 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n ? 0 : -1;
}
#endif

/* IP de la interfaz con la que se sale a Internet (sin mandar nada: connect de UDP) */
int smnet_udp_lan_ip(char out[16]) {
    int fd = smnet_udp_open(0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    int ok = connect(fd, (struct sockaddr*)&a, sizeof a) == 0;
    struct sockaddr_in me;
    socklen_t l = sizeof me;
    if (ok) ok = getsockname(fd, (struct sockaddr*)&me, &l) == 0;
    smnet_udp_close(fd);
    if (!ok || !inet_ntop(AF_INET, &me.sin_addr, out, 16)) { strcpy(out, "127.0.0.1"); return -1; }
    return 0;
}
