// SPDX-License-Identifier: GPL-3.0-or-later
/* Host sockets for online ad hoc play (hle_adhoc.c), Winsock and BSD alike. Every socket is
 * non-blocking; a handle is a small index (>= 0). IPv4 addresses are in network order (the
 * raw bytes of the address, as the lobby protocol carries them); ports are in host order. */
#ifndef SR_ADHOC_SOCK_H
#define SR_ADHOC_SOCK_H

#include <stdint.h>

#define AS_AGAIN   (-2)   /* would block / still in progress */
#define AS_INUSE   (-3)   /* address already in use */
#define AS_CLOSED  (-4)   /* the peer closed or reset the connection */
#define AS_FAIL    (-1)

uint32_t as_resolve(const char *host);             /* 0 if it does not resolve */
int  as_udp_open(uint32_t ip, uint16_t port);       /* bound; handle or AS_INUSE/AS_FAIL */
int  as_tcp_open(uint32_t ip, uint16_t port);       /* bound, unconnected; handle or error */
int  as_tcp_listen(int h, int backlog);
int  as_tcp_connect(int h, uint32_t ip, uint16_t port);   /* 0, AS_AGAIN or AS_FAIL */
int  as_tcp_connected(int h);                       /* 1 connected, AS_AGAIN, AS_FAIL refused */
int  as_tcp_accept(int h, uint32_t *ip, uint16_t *port);  /* new handle or AS_AGAIN/AS_FAIL */
int  as_send(int h, const void *data, int len);     /* bytes sent, AS_AGAIN or AS_CLOSED */
int  as_recv(int h, void *data, int len);           /* bytes, AS_CLOSED (0 bytes = closed) or AS_AGAIN */
int  as_sendto(int h, const void *data, int len, uint32_t ip, uint16_t port);
/* One datagram; peek leaves it queued. Returns its full length (may exceed len), or AS_AGAIN. */
int  as_recvfrom(int h, void *data, int len, uint32_t *ip, uint16_t *port, int peek);
void as_close(int h);

#endif
