// SPDX-License-Identifier: GPL-3.0-or-later
/* Host sockets for online ad hoc play: see adhoc_sock.h. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
typedef int socklen_t_;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
static int sock_errno(void) { return WSAGetLastError(); }
#define E_AGAIN(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS || (e) == WSAEALREADY)
#define E_INUSE(e) ((e) == WSAEADDRINUSE)
#define E_RESET(e) ((e) == WSAECONNRESET || (e) == WSAECONNABORTED || (e) == WSAENOTCONN)
#define SEND_FLAGS 0
#else
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
typedef socklen_t socklen_t_;
#define BAD_SOCK (-1)
#define sock_close close
static int sock_errno(void) { return errno; }
#define E_AGAIN(e) ((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == EINPROGRESS || (e) == EALREADY)
#define E_INUSE(e) ((e) == EADDRINUSE)
#define E_RESET(e) ((e) == ECONNRESET || (e) == EPIPE || (e) == ENOTCONN)
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif
#endif

#include <stdint.h>
#include <string.h>

#include "adhoc_sock.h"

#define AS_MAX 64
static sock_t s_tab[AS_MAX];
static int s_used[AS_MAX];
static char s_scratch[65536];   /* one datagram, whole */

static int as_ready(void) {
#ifdef _WIN32
    static int started;
    if (!started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
        started = 1;
    }
#endif
    return 1;
}

static int as_add(sock_t s) {
    for (int i = 0; i < AS_MAX; i++)
        if (!s_used[i]) { s_used[i] = 1; s_tab[i] = s; return i; }
    sock_close(s);
    return AS_FAIL;
}

static sock_t as_get(int h) { return h >= 0 && h < AS_MAX && s_used[h] ? s_tab[h] : BAD_SOCK; }

static void as_nonblock(sock_t s) {
#ifdef _WIN32
    u_long one = 1;
    ioctlsocket(s, FIONBIO, &one);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
#endif
}

static struct sockaddr_in as_addr(uint32_t ip, uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = htons(port);
    return a;
}

uint32_t as_resolve(const char *host) {
    if (!as_ready() || !host || !*host) return 0;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return 0;
    uint32_t ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return ip;
}

static int as_open(int type, uint32_t ip, uint16_t port) {
    if (!as_ready()) return AS_FAIL;
    sock_t s = socket(AF_INET, type, 0);
    if (s == BAD_SOCK) return AS_FAIL;
    int one = 1;
    /* TCP: a port a closed connection left in TIME_WAIT can be bound again at once. */
    if (type == SOCK_STREAM) {
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    } else {
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    }
    struct sockaddr_in a = as_addr(ip, port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        int e = sock_errno();
        sock_close(s);
        return E_INUSE(e) ? AS_INUSE : AS_FAIL;
    }
    as_nonblock(s);
    return as_add(s);
}

int as_udp_open(uint32_t ip, uint16_t port) { return as_open(SOCK_DGRAM, ip, port); }
int as_tcp_open(uint32_t ip, uint16_t port) { return as_open(SOCK_STREAM, ip, port); }

int as_tcp_listen(int h, int backlog) {
    sock_t s = as_get(h);
    return s != BAD_SOCK && listen(s, backlog > 0 ? backlog : 8) == 0 ? 0 : AS_FAIL;
}

int as_tcp_connect(int h, uint32_t ip, uint16_t port) {
    sock_t s = as_get(h);
    if (s == BAD_SOCK) return AS_FAIL;
    struct sockaddr_in a = as_addr(ip, port);
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) return 0;
    int e = sock_errno();
    return E_AGAIN(e) ? AS_AGAIN : AS_FAIL;
}

int as_tcp_connected(int h) {
    sock_t s = as_get(h);
    if (s == BAD_SOCK) return AS_FAIL;
    fd_set w, x;
    FD_ZERO(&w);
    FD_ZERO(&x);
    FD_SET(s, &w);
    FD_SET(s, &x);
    struct timeval tv = {0, 0};
    int r = select((int)s + 1, NULL, &w, &x, &tv);
    if (r == 0) return AS_AGAIN;
    int err = 0;
    socklen_t_ len = sizeof err;
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
    return r < 0 || err || FD_ISSET(s, &x) ? AS_FAIL : 1;
}

int as_tcp_accept(int h, uint32_t *ip, uint16_t *port) {
    sock_t s = as_get(h);
    if (s == BAD_SOCK) return AS_FAIL;
    struct sockaddr_in a;
    socklen_t_ len = sizeof a;
    sock_t c = accept(s, (struct sockaddr *)&a, &len);
    if (c == BAD_SOCK) return E_AGAIN(sock_errno()) ? AS_AGAIN : AS_FAIL;
    as_nonblock(c);
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    if (ip) *ip = a.sin_addr.s_addr;
    if (port) *port = ntohs(a.sin_port);
    return as_add(c);
}

int as_send(int h, const void *data, int len) {
    sock_t s = as_get(h);
    if (s == BAD_SOCK) return AS_CLOSED;
    int n = (int)send(s, (const char *)data, len, SEND_FLAGS);
    if (n >= 0) return n;
    int e = sock_errno();
    return E_AGAIN(e) ? AS_AGAIN : AS_CLOSED;
}

int as_recv(int h, void *data, int len) {
    sock_t s = as_get(h);
    if (s == BAD_SOCK) return AS_CLOSED;
    int n = (int)recv(s, (char *)data, len, 0);
    if (n > 0) return n;
    if (n == 0) return AS_CLOSED;
    int e = sock_errno();
    return E_AGAIN(e) ? AS_AGAIN : AS_CLOSED;
}

int as_sendto(int h, const void *data, int len, uint32_t ip, uint16_t port) {
    sock_t s = as_get(h);
    if (s == BAD_SOCK) return AS_FAIL;
    struct sockaddr_in a = as_addr(ip, port);
    int n = (int)sendto(s, (const char *)data, len, SEND_FLAGS, (struct sockaddr *)&a, sizeof a);
    return n >= 0 ? n : (E_AGAIN(sock_errno()) ? AS_AGAIN : AS_FAIL);
}

int as_recvfrom(int h, void *data, int len, uint32_t *ip, uint16_t *port, int peek) {
    sock_t s = as_get(h);
    if (s == BAD_SOCK) return AS_FAIL;
    struct sockaddr_in a;
    socklen_t_ alen = sizeof a;
    int n = (int)recvfrom(s, s_scratch, (int)sizeof s_scratch, peek ? MSG_PEEK : 0,
                          (struct sockaddr *)&a, &alen);
    if (n < 0) {
        int e = sock_errno();
        /* Windows reports an ICMP port-unreachable for an earlier datagram here: skip it. */
        return E_AGAIN(e) || E_RESET(e) ? AS_AGAIN : AS_FAIL;
    }
    if (data && len > 0) memcpy(data, s_scratch, (size_t)(n < len ? n : len));
    if (ip) *ip = a.sin_addr.s_addr;
    if (port) *port = ntohs(a.sin_port);
    return n;
}

void as_close(int h) {
    sock_t s = as_get(h);
    if (s == BAD_SOCK) return;
    sock_close(s);
    s_used[h] = 0;
}
