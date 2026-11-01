#ifndef TZ_UDP_H
#define TZ_UDP_H

#include "runtime/library/net/net/net.h"

#ifdef _WIN32

typedef struct UdpDtlsOps {
  int (*input)(void *state, const SOCKADDR_STORAGE *from, int from_length, const uint8_t *data,
               uint32_t length, uint8_t **plain, uint32_t *plain_length);
  void (*seal)(void *state, const SOCKADDR_STORAGE *to, int to_length, const uint8_t *data,
               size_t length, TzPromise *promise, TzMaker make);
  void (*close)(void *state);
  void (*release)(void *state);
} UdpDtlsOps;

void udp_attach_dtls(void *sock, void *state, const UdpDtlsOps *ops);
void udp_send_datagram(void *sock, const SOCKADDR_STORAGE *to, int to_length, const uint8_t *data,
                       size_t length, TzPromise *promise, TzMaker make);
void udp_send_now(void *sock, const SOCKADDR_STORAGE *to, int to_length, const uint8_t *data,
                  size_t length);
int udp_peer(void *sock, SOCKADDR_STORAGE *out, int *length);

#endif

#endif
