#include "runtime/core/alloc/alloc.h"
#include "runtime/core/async/async.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/io/io.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include "runtime/library/net/net/net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UDP_NOT_FOUND (-3008)
#define UDP_TRY_AGAIN (-3001)
#define UDP_MAX 65536

enum {
  UDP_BROADCAST = 1,
  UDP_TTL = 2,
  UDP_MULTICAST_TTL = 3,
  UDP_MULTICAST_LOOP = 4,
  UDP_RECEIVE_BUFFER = 5,
  UDP_SEND_BUFFER = 6,
};

static TzStr udp_ascii(const char *text) { return tz_str_from_ascii(text, strlen(text)); }

static void *udp_failure(TzMaker make, const char *code, const char *message, const char *syscall) {
  TzSystemErrorParts parts;
  memset(&parts, 0, sizeof parts);
  parts.message = net_utf8(message);
  parts.errno_value = -1;
  parts.code = udp_ascii(code);
  parts.syscall = udp_ascii(syscall);

  return make(&parts);
}

#ifdef _WIN32

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

typedef struct UdpSocket {
  int64_t rc;
  const TzLayout *layout;
  SOCKET s;
  int32_t family;
  uint8_t closed, orphan, connected, dual;
  int32_t ops;
  int32_t pending_error;
  NetOp rop;
  TzPromise *received;
  TzMaker make;
  SOCKADDR_STORAGE from;
  INT from_length;
  DWORD flags;
  char sender[64];
  int32_t sender_port;
  uint8_t data[UDP_MAX];
} UdpSocket;

typedef struct UdpSend {
  NetOp op;
  UdpSocket *u;
  TzPromise *promise;
  TzMaker make;
  uint32_t length;
  char where[96];
  uint8_t data[];
} UdpSend;

typedef struct UdpLookup {
  NetOp op;
  TzPromise *promise;
  TzMaker make;
  ADDRINFOEXW *found;
  HANDLE cancel;
  int32_t family;
  char host[256];
} UdpLookup;

static void udp_drop(void *object);
static const TzLayout udp_layout = {udp_drop, NULL, 0, 0, "Socket", 6, 0};

static void udp_free(UdpSocket *u) { tz_obj_free(u); }

static void udp_settle(UdpSocket *u) {
  if (u->orphan && u->ops == 0) {
    udp_free(u);
  }
}

static void udp_close_socket(UdpSocket *u) {
  if (u->closed) {
    return;
  }
  u->closed = 1;
  closesocket(u->s);
}

static void udp_drop(void *object) {
  TZ_ORDERED();
  UdpSocket *u = (UdpSocket *)object;
  udp_close_socket(u);
  if (u->received) {
    TzPromise *p = u->received;
    u->received = NULL;
    tz_obj_release((TzObject *)p);
  }
  u->orphan = 1;
  udp_settle(u);
}

static void address_of(const SOCKADDR_STORAGE *sa, char *out, size_t size, int32_t *port) {
  const SOCKADDR_IN6 *v6 = (const SOCKADDR_IN6 *)sa;
  if (sa->ss_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) {
    SOCKADDR_IN v4;
    memset(&v4, 0, sizeof v4);
    v4.sin_family = AF_INET;
    memcpy(&v4.sin_addr, &v6->sin6_addr.s6_addr[12], 4);
    net_address_text((const SOCKADDR *)&v4, out, size);
  } else {
    net_address_text((const SOCKADDR *)sa, out, size);
  }
  *port = net_address_port((const SOCKADDR *)sa);
}

static void map_to_v6(SOCKADDR_STORAGE *address, int *length) {
  SOCKADDR_IN v4 = *(SOCKADDR_IN *)address;
  SOCKADDR_IN6 *v6 = (SOCKADDR_IN6 *)address;
  memset(address, 0, sizeof *address);
  v6->sin6_family = AF_INET6;
  v6->sin6_port = v4.sin_port;
  v6->sin6_addr.s6_addr[10] = 0xFF;
  v6->sin6_addr.s6_addr[11] = 0xFF;
  memcpy(&v6->sin6_addr.s6_addr[12], &v4.sin_addr, 4);
  *length = sizeof(SOCKADDR_IN6);
}

static int target_of(UdpSocket *u, const char *host, int64_t port, SOCKADDR_STORAGE *address,
                     int *length) {
  if (!net_parse_literal(host, (int)port, address, length)) {
    return WSAEINVAL;
  }
  if (address->ss_family == u->family) {
    return 0;
  }
  if (address->ss_family == AF_INET && u->dual) {
    map_to_v6(address, length);
    return 0;
  }

  return WSAEAFNOSUPPORT;
}

static void connect_resets(UdpSocket *u, int on) {
  BOOL value = on ? TRUE : FALSE;
  DWORD bytes = 0;
  WSAIoctl(u->s, SIO_UDP_CONNRESET, &value, sizeof value, NULL, 0, &bytes, NULL, NULL);
}

void *tz_udp_open(int64_t family, void *address_obj, uint64_t address_meta, int64_t port,
                  int32_t reuse, int32_t ipv6_only, TzMaker make) {
  TZ_ORDERED();
  net_start();
  char host[256];
  if (!net_ascii_of(address_obj, address_meta, host, sizeof host)) {
    tz_failed = udp_failure(make, "ERR_INVALID_ADDRESS", "the address must be ASCII", "bind");
    return NULL;
  }
  if (_stricmp(host, "localhost") == 0) {
    strcpy(host, family == 6 ? "::1" : "127.0.0.1");
  }
  SOCKADDR_STORAGE address;
  int length = 0;
  memset(&address, 0, sizeof address);
  if (host[0] == 0) {
    if (family == 4) {
      SOCKADDR_IN *v4 = (SOCKADDR_IN *)&address;
      v4->sin_family = AF_INET;
      v4->sin_port = htons((u_short)port);
      length = sizeof(SOCKADDR_IN);
    } else {
      SOCKADDR_IN6 *v6 = (SOCKADDR_IN6 *)&address;
      v6->sin6_family = AF_INET6;
      v6->sin6_port = htons((u_short)port);
      length = sizeof(SOCKADDR_IN6);
    }
  } else if (!net_parse_literal(host, (int)port, &address, &length)) {
    tz_failed =
        udp_failure(make, "ERR_INVALID_ADDRESS", "the address is not an IP address", "bind");
    return NULL;
  }
  char where[300];
  char text[64];
  net_address_text((SOCKADDR *)&address, text, sizeof text);
  snprintf(where, sizeof where, "%s:%lld", text, (long long)port);
  if ((family == 4 && address.ss_family != AF_INET) ||
      (family == 6 && address.ss_family != AF_INET6)) {
    tz_failed = net_system_error(make, WSAEAFNOSUPPORT, "bind", where, 1);
    return NULL;
  }
  int af = address.ss_family;
  SOCKET s = WSASocketW(af, SOCK_DGRAM, IPPROTO_UDP, NULL, 0,
                        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  if (s == INVALID_SOCKET) {
    tz_failed = net_system_error(make, WSAGetLastError(), "bind", where, 1);
    return NULL;
  }
  int dual = af == AF_INET6 && !ipv6_only;
  if (af == AF_INET6) {
    DWORD only = dual ? 0 : 1;
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&only, sizeof only);
  }
  if (reuse) {
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
  }
  if (bind(s, (SOCKADDR *)&address, length) == SOCKET_ERROR || !net_attach(s)) {
    int error = WSAGetLastError();
    closesocket(s);
    tz_failed = net_system_error(make, error, "bind", where, 1);
    return NULL;
  }
  UdpSocket *u = (UdpSocket *)tz_obj_alloc(sizeof(UdpSocket));
  memset(u, 0, offsetof(UdpSocket, data));
  u->rc = 1;
  u->layout = &udp_layout;
  u->s = s;
  u->family = af;
  u->dual = (uint8_t)dual;
  u->make = make;
  connect_resets(u, 0);

  return u;
}

int64_t tz_udp_option(void *sock, int64_t option, int64_t value, TzMaker make) {
  TZ_ORDERED();
  UdpSocket *u = (UdpSocket *)sock;
  DWORD number = (DWORD)value;
  int rc = 0;
  const char *name = "setsockopt";
  switch (option) {
  case UDP_BROADCAST: {
    BOOL on = value ? TRUE : FALSE;
    rc = setsockopt(u->s, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
    name = "setBroadcast";
    break;
  }
  case UDP_TTL:
    name = "setTTL";
    if (u->family == AF_INET6) {
      rc = setsockopt(u->s, IPPROTO_IPV6, IPV6_UNICAST_HOPS, (const char *)&number, sizeof number);
    }
    if (u->family == AF_INET || u->dual) {
      int v4 = setsockopt(u->s, IPPROTO_IP, IP_TTL, (const char *)&number, sizeof number);
      rc = u->family == AF_INET ? v4 : rc;
    }
    break;
  case UDP_MULTICAST_TTL:
    name = "setMulticastTTL";
    if (u->family == AF_INET6) {
      rc =
          setsockopt(u->s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (const char *)&number, sizeof number);
    }
    if (u->family == AF_INET || u->dual) {
      int v4 = setsockopt(u->s, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&number, sizeof number);
      rc = u->family == AF_INET ? v4 : rc;
    }
    break;
  case UDP_MULTICAST_LOOP:
    name = "setMulticastLoopback";
    if (u->family == AF_INET6) {
      rc =
          setsockopt(u->s, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (const char *)&number, sizeof number);
    }
    if (u->family == AF_INET || u->dual) {
      int v4 =
          setsockopt(u->s, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&number, sizeof number);
      rc = u->family == AF_INET ? v4 : rc;
    }
    break;
  case UDP_RECEIVE_BUFFER: {
    int size = (int)value;
    rc = setsockopt(u->s, SOL_SOCKET, SO_RCVBUF, (const char *)&size, sizeof size);
    name = "setRecvBufferSize";
    break;
  }
  case UDP_SEND_BUFFER: {
    int size = (int)value;
    rc = setsockopt(u->s, SOL_SOCKET, SO_SNDBUF, (const char *)&size, sizeof size);
    name = "setSendBufferSize";
    break;
  }
  default:
    break;
  }
  if (rc == SOCKET_ERROR) {
    tz_failed = net_system_error(make, WSAGetLastError(), name, NULL, 1);
  }

  return 0;
}

static int interface_index(const char *text) {
  const char *digits = text[0] == '%' ? text + 1 : text;
  if (!digits[0]) {
    return 0;
  }
  char *end = NULL;
  long index = strtol(digits, &end, 10);

  return end && *end == 0 && index >= 0 ? (int)index : 0;
}

int64_t tz_udp_multicast_interface(void *sock, void *iface_obj, uint64_t iface_meta, TzMaker make) {
  TZ_ORDERED();
  UdpSocket *u = (UdpSocket *)sock;
  char iface[128];
  if (!net_ascii_of(iface_obj, iface_meta, iface, sizeof iface)) {
    tz_failed = udp_failure(make, "ERR_INVALID_ADDRESS", "the interface must be ASCII",
                            "setMulticastInterface");
    return 0;
  }
  SOCKADDR_STORAGE address;
  int length = 0;
  int rc;
  if (net_parse_literal(iface, 0, &address, &length) && address.ss_family == AF_INET) {
    IN_ADDR in = ((SOCKADDR_IN *)&address)->sin_addr;
    rc = setsockopt(u->s, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&in, sizeof in);
  } else {
    DWORD index = (DWORD)interface_index(iface);
    rc = setsockopt(u->s, IPPROTO_IPV6, IPV6_MULTICAST_IF, (const char *)&index, sizeof index);
  }
  if (rc == SOCKET_ERROR) {
    tz_failed = net_system_error(make, WSAGetLastError(), "setMulticastInterface", iface, 1);
  }

  return 0;
}

int64_t tz_udp_membership(void *sock, void *group_obj, uint64_t group_meta, void *iface_obj,
                          uint64_t iface_meta, void *source_obj, uint64_t source_meta, int32_t join,
                          TzMaker make) {
  TZ_ORDERED();
  UdpSocket *u = (UdpSocket *)sock;
  char group[128], iface[128], source[128];
  const char *syscall = join ? "addMembership" : "dropMembership";
  if (!net_ascii_of(group_obj, group_meta, group, sizeof group) ||
      !net_ascii_of(iface_obj, iface_meta, iface, sizeof iface) ||
      !net_ascii_of(source_obj, source_meta, source, sizeof source)) {
    tz_failed = udp_failure(make, "ERR_INVALID_ADDRESS", "the addresses must be ASCII", syscall);
    return 0;
  }
  SOCKADDR_STORAGE g, i, s;
  int length = 0;
  if (!net_parse_literal(group, 0, &g, &length)) {
    tz_failed = net_system_error(make, WSAEINVAL, syscall, group, 1);
    return 0;
  }
  int rc;
  if (g.ss_family == AF_INET) {
    IN_ADDR any;
    memset(&any, 0, sizeof any);
    IN_ADDR via = any;
    if (iface[0] && net_parse_literal(iface, 0, &i, &length) && i.ss_family == AF_INET) {
      via = ((SOCKADDR_IN *)&i)->sin_addr;
    }
    if (source[0]) {
      if (!net_parse_literal(source, 0, &s, &length) || s.ss_family != AF_INET) {
        tz_failed = net_system_error(make, WSAEINVAL, syscall, source, 1);
        return 0;
      }
      struct ip_mreq_source request;
      memset(&request, 0, sizeof request);
      request.imr_multiaddr = ((SOCKADDR_IN *)&g)->sin_addr;
      request.imr_sourceaddr = ((SOCKADDR_IN *)&s)->sin_addr;
      request.imr_interface = via;
      rc = setsockopt(u->s, IPPROTO_IP, join ? IP_ADD_SOURCE_MEMBERSHIP : IP_DROP_SOURCE_MEMBERSHIP,
                      (const char *)&request, sizeof request);
    } else {
      struct ip_mreq request;
      memset(&request, 0, sizeof request);
      request.imr_multiaddr = ((SOCKADDR_IN *)&g)->sin_addr;
      request.imr_interface = via;
      rc = setsockopt(u->s, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                      (const char *)&request, sizeof request);
    }
  } else {
    if (source[0]) {
      tz_failed =
          udp_failure(make, "ERR_NOT_SUPPORTED",
                      "a membership with a source is supported only for IPv4 groups", syscall);
      return 0;
    }
    struct ipv6_mreq request;
    memset(&request, 0, sizeof request);
    request.ipv6mr_multiaddr = ((SOCKADDR_IN6 *)&g)->sin6_addr;
    request.ipv6mr_interface = (ULONG)interface_index(iface);
    rc = setsockopt(u->s, IPPROTO_IPV6, join ? IPV6_ADD_MEMBERSHIP : IPV6_DROP_MEMBERSHIP,
                    (const char *)&request, sizeof request);
  }
  if (rc == SOCKET_ERROR) {
    tz_failed = net_system_error(make, WSAGetLastError(), syscall, group, 1);
  }

  return 0;
}

static void receive_done(TzWork *w);

static void receive_settle(UdpSocket *u, int32_t error, uint32_t bytes) {
  if (error == 0) {
    TzBytes *data = net_bytes(u->data, bytes);
    address_of(&u->from, u->sender, sizeof u->sender, &u->sender_port);
    net_fulfill(&u->received, &data, sizeof data);
    return;
  }
  if (u->closed || error == ERROR_OPERATION_ABORTED || error == WSA_OPERATION_ABORTED) {
    TzBytes *none = NULL;
    net_fulfill(&u->received, &none, sizeof none);
    return;
  }
  net_reject(&u->received, net_system_error(u->make, error, "recvmsg", NULL, 1));
}

static int receive_retry(UdpSocket *u, int32_t error) {
  if (u->closed) {
    return 0;
  }
  if (error == WSAECONNRESET || error == WSAENETRESET || error == ERROR_PORT_UNREACHABLE ||
      error == ERROR_CONNECTION_REFUSED) {
    if (u->connected) {
      u->pending_error = WSAECONNREFUSED;
    }
    return 1;
  }

  return error == WSAEMSGSIZE || error == ERROR_MORE_DATA;
}

static void receive_next(UdpSocket *u) {
  for (;;) {
    if (u->closed) {
      receive_settle(u, ERROR_OPERATION_ABORTED, 0);
      return;
    }
    WSABUF buffer;
    buffer.len = UDP_MAX;
    buffer.buf = (CHAR *)u->data;
    u->from_length = sizeof u->from;
    u->flags = 0;
    DWORD got = 0;
    net_op_begin(&u->rop, receive_done, 0);
    ++u->ops;
    int rc = WSARecvFrom(u->s, &buffer, 1, &got, &u->flags, (SOCKADDR *)&u->from, &u->from_length,
                         &u->rop.ov, NULL);
    int32_t error = rc == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (error == WSA_IO_PENDING) {
      net_op_wait(&u->rop);
      return;
    }
    --u->ops;
    if (error && receive_retry(u, error)) {
      continue;
    }
    receive_settle(u, error, got);
    return;
  }
}

static void receive_done(TzWork *w) {
  UdpSocket *u = CONTAINING_RECORD(CONTAINING_RECORD(w, NetOp, work), UdpSocket, rop);
  --u->ops;
  if (u->orphan) {
    udp_settle(u);
    return;
  }
  int32_t error = net_op_error(&u->rop, u->s, u->closed);
  if (error && receive_retry(u, error)) {
    receive_next(u);
    return;
  }
  receive_settle(u, error, u->rop.bytes);
}

void tz_udp_receive(TzPromise *p, void *sock, TzMaker make) {
  TZ_ORDERED();
  UdpSocket *u = (UdpSocket *)sock;
  tz_ref_retain(p);
  u->make = make;
  if (u->received) {
    TzPromise *busy = p;
    net_reject(&busy,
               udp_failure(make, "ERR_SOCKET_BUSY", "the socket is receiving already", "recvmsg"));
    return;
  }
  u->received = p;
  receive_next(u);
}

void tz_udp_sender_address(TzStr *out, void *sock, TzMaker make) {
  (void)make;
  UdpSocket *u = (UdpSocket *)sock;
  *out = tz_str_from_ascii(u->sender, strlen(u->sender));
}

int64_t tz_udp_sender_port(void *sock, TzMaker make) {
  (void)make;

  return ((UdpSocket *)sock)->sender_port;
}

static void send_done(TzWork *w);

static void send_settle(UdpSend *r, int32_t error) {
  UdpSocket *u = r->u;
  if (error) {
    net_reject(&r->promise, net_system_error(r->make, error, "send", r->where, 1));
  } else {
    net_fulfill(&r->promise, NULL, 0);
  }
  tz_free(r);
  udp_settle(u);
}

static void send_done(TzWork *w) {
  UdpSend *r = (UdpSend *)CONTAINING_RECORD(w, NetOp, work);
  UdpSocket *u = r->u;
  --u->ops;
  send_settle(r, net_op_error(&r->op, u->s, u->closed));
}

void tz_udp_send(TzPromise *p, void *sock, void *data, void *address_obj, uint64_t address_meta,
                 int64_t port, TzMaker make) {
  TZ_ORDERED();
  UdpSocket *u = (UdpSocket *)sock;
  tz_ref_retain(p);
  TzPromise *promise = p;
  if (u->closed) {
    net_reject(&promise, udp_failure(make, "ERR_SOCKET_CLOSED", "the socket is closed", "send"));
    return;
  }
  if (u->pending_error) {
    int32_t error = u->pending_error;
    u->pending_error = 0;
    net_reject(&promise, net_system_error(make, error, "send", NULL, 1));
    return;
  }
  char host[128];
  if (!net_ascii_of(address_obj, address_meta, host, sizeof host)) {
    net_reject(&promise,
               udp_failure(make, "ERR_INVALID_ADDRESS", "the address must be ASCII", "send"));
    return;
  }
  SOCKADDR_STORAGE address;
  int length = 0;
  if (host[0]) {
    int error = target_of(u, host, port, &address, &length);
    if (error) {
      char where[160];
      snprintf(where, sizeof where, "%s:%lld", host, (long long)port);
      net_reject(&promise, net_system_error(make, error, "send", where, 1));
      return;
    }
  }
  TzBytes *bytes = (TzBytes *)data;
  size_t size = (size_t)tz_bytes_byte_length(bytes);
  UdpSend *r = (UdpSend *)tz_alloc(sizeof(UdpSend) + size);
  memset(r, 0, sizeof *r);
  r->u = u;
  r->promise = promise;
  r->make = make;
  r->length = (uint32_t)size;
  if (size) {
    memcpy(r->data, bytes->data, size);
  }
  if (host[0]) {
    snprintf(r->where, sizeof r->where, "%s:%lld", host, (long long)port);
  }
  WSABUF buffer;
  buffer.len = (ULONG)size;
  buffer.buf = (CHAR *)r->data;
  DWORD sent = 0;
  net_op_begin(&r->op, send_done, 0);
  ++u->ops;
  int rc =
      host[0] ? WSASendTo(u->s, &buffer, 1, &sent, 0, (SOCKADDR *)&address, length, &r->op.ov, NULL)
              : WSASend(u->s, &buffer, 1, &sent, 0, &r->op.ov, NULL);
  int32_t error = rc == SOCKET_ERROR ? WSAGetLastError() : 0;
  if (error == WSA_IO_PENDING) {
    net_op_wait(&r->op);
    return;
  }
  --u->ops;
  send_settle(r, error);
}

int64_t tz_udp_connect(void *sock, void *address_obj, uint64_t address_meta, int64_t port,
                       TzMaker make) {
  TZ_ORDERED();
  UdpSocket *u = (UdpSocket *)sock;
  char host[128];
  if (!net_ascii_of(address_obj, address_meta, host, sizeof host)) {
    tz_failed = udp_failure(make, "ERR_INVALID_ADDRESS", "the address must be ASCII", "connect");
    return 0;
  }
  char where[160];
  snprintf(where, sizeof where, "%s:%lld", host, (long long)port);
  SOCKADDR_STORAGE address;
  int length = 0;
  int error = target_of(u, host, port, &address, &length);
  if (!error && connect(u->s, (SOCKADDR *)&address, length) == SOCKET_ERROR) {
    error = WSAGetLastError();
  }
  if (error) {
    tz_failed = net_system_error(make, error, "connect", where, 1);
    return 0;
  }
  u->connected = 1;
  u->pending_error = 0;
  connect_resets(u, 1);

  return 0;
}

void tz_udp_disconnect(void *sock, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  UdpSocket *u = (UdpSocket *)sock;
  SOCKADDR_STORAGE none;
  memset(&none, 0, sizeof none);
  none.ss_family = AF_UNSPEC;
  connect(u->s, (SOCKADDR *)&none, sizeof none);
  u->connected = 0;
  u->pending_error = 0;
  connect_resets(u, 0);
}

static void local_of(UdpSocket *u, char *out, size_t size, int32_t *port) {
  SOCKADDR_STORAGE bound;
  int length = sizeof bound;
  out[0] = 0;
  *port = 0;
  if (getsockname(u->s, (SOCKADDR *)&bound, &length) == SOCKET_ERROR) {
    return;
  }
  address_of(&bound, out, size, port);
}

void tz_udp_local_address(TzStr *out, void *sock, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  char text[64];
  int32_t port;
  local_of((UdpSocket *)sock, text, sizeof text, &port);
  *out = tz_str_from_ascii(text, strlen(text));
}

int64_t tz_udp_local_port(void *sock, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  char text[64];
  int32_t port;
  local_of((UdpSocket *)sock, text, sizeof text, &port);

  return port;
}

void tz_udp_close(void *sock, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  udp_close_socket((UdpSocket *)sock);
}

static void lookup_done(TzWork *w) {
  UdpLookup *r = (UdpLookup *)CONTAINING_RECORD(w, NetOp, work);
  char text[64];
  text[0] = 0;
  for (int pass = 0; pass < 2 && !text[0]; ++pass) {
    for (ADDRINFOEXW *a = r->found; a; a = a->ai_next) {
      int wanted = r->family == 6   ? AF_INET6
                   : r->family == 4 ? AF_INET
                   : pass == 0      ? AF_INET
                                    : AF_INET6;
      if (a->ai_family == wanted) {
        net_address_text(a->ai_addr, text, sizeof text);
        break;
      }
    }
  }
  if (r->found) {
    FreeAddrInfoExW(r->found);
  }
  if (text[0]) {
    TzStr found = tz_str_from_ascii(text, strlen(text));
    net_fulfill(&r->promise, &found, sizeof found);
  } else {
    int32_t error = r->op.error == WSATRY_AGAIN ? UDP_TRY_AGAIN : UDP_NOT_FOUND;
    net_reject(&r->promise, net_system_error(r->make, error, "getaddrinfo", r->host, 0));
  }
  tz_free(r);
}

static void lookup_posted(OVERLAPPED_ENTRY *entry) {
  NetOp *op = CONTAINING_RECORD(entry->lpOverlapped, NetOp, ov);
  tz_io_due(&op->work);
}

static void CALLBACK lookup_found(DWORD error, DWORD bytes, LPWSAOVERLAPPED overlapped) {
  (void)bytes;
  NetOp *op = CONTAINING_RECORD(overlapped, NetOp, ov);
  op->error = (int32_t)error;
  PostQueuedCompletionStatus((HANDLE)tz_io_port_handle(), 0, (ULONG_PTR)lookup_posted, overlapped);
}

void tz_udp_lookup(TzPromise *p, void *host_obj, uint64_t host_meta, int64_t family, TzMaker make) {
  TZ_ORDERED();
  net_start();
  UdpLookup *r = (UdpLookup *)tz_alloc(sizeof(UdpLookup));
  memset(r, 0, sizeof *r);
  tz_ref_retain(p);
  r->promise = p;
  r->make = make;
  r->family = (int32_t)family;
  if (!net_ascii_of(host_obj, host_meta, r->host, sizeof r->host)) {
    net_reject(&r->promise, udp_failure(make, "ERR_INVALID_ADDRESS", "the host name must be ASCII",
                                        "getaddrinfo"));
    tz_free(r);
    return;
  }
  wchar_t wide[256];
  MultiByteToWideChar(CP_UTF8, 0, r->host, -1, wide, 256);
  ADDRINFOEXW hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = family == 4 ? AF_INET : family == 6 ? AF_INET6 : AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  net_op_begin(&r->op, lookup_done, 0);
  INT rc = GetAddrInfoExW(wide, NULL, NS_ALL, NULL, &hints, &r->found, NULL, &r->op.ov,
                          lookup_found, &r->cancel);
  if (rc == WSA_IO_PENDING) {
    net_op_wait(&r->op);
    return;
  }
  r->op.error = rc;
  lookup_done(&r->op.work);
}

#else

TZ_NORETURN static void unsupported(void) {
  tz_panicf("UDP is not supported on this system yet (docs/net.md, section 7)");
}

void *tz_udp_open(int64_t family, void *address_obj, uint64_t address_meta, int64_t port,
                  int32_t reuse, int32_t ipv6_only, TzMaker make) {
  (void)family;
  (void)address_obj;
  (void)address_meta;
  (void)port;
  (void)reuse;
  (void)ipv6_only;
  (void)make;
  unsupported();
}

int64_t tz_udp_option(void *sock, int64_t option, int64_t value, TzMaker make) {
  (void)sock;
  (void)option;
  (void)value;
  (void)make;
  unsupported();
}

int64_t tz_udp_multicast_interface(void *sock, void *iface_obj, uint64_t iface_meta, TzMaker make) {
  (void)sock;
  (void)iface_obj;
  (void)iface_meta;
  (void)make;
  unsupported();
}

int64_t tz_udp_membership(void *sock, void *group_obj, uint64_t group_meta, void *iface_obj,
                          uint64_t iface_meta, void *source_obj, uint64_t source_meta, int32_t join,
                          TzMaker make) {
  (void)sock;
  (void)group_obj;
  (void)group_meta;
  (void)iface_obj;
  (void)iface_meta;
  (void)source_obj;
  (void)source_meta;
  (void)join;
  (void)make;
  unsupported();
}

void tz_udp_receive(TzPromise *p, void *sock, TzMaker make) {
  (void)p;
  (void)sock;
  (void)make;
  unsupported();
}

void tz_udp_sender_address(TzStr *out, void *sock, TzMaker make) {
  (void)out;
  (void)sock;
  (void)make;
  unsupported();
}

int64_t tz_udp_sender_port(void *sock, TzMaker make) {
  (void)sock;
  (void)make;
  unsupported();
}

void tz_udp_send(TzPromise *p, void *sock, void *data, void *address_obj, uint64_t address_meta,
                 int64_t port, TzMaker make) {
  (void)p;
  (void)sock;
  (void)data;
  (void)address_obj;
  (void)address_meta;
  (void)port;
  (void)make;
  unsupported();
}

int64_t tz_udp_connect(void *sock, void *address_obj, uint64_t address_meta, int64_t port,
                       TzMaker make) {
  (void)sock;
  (void)address_obj;
  (void)address_meta;
  (void)port;
  (void)make;
  unsupported();
}

void tz_udp_disconnect(void *sock, TzMaker make) {
  (void)sock;
  (void)make;
  unsupported();
}

void tz_udp_local_address(TzStr *out, void *sock, TzMaker make) {
  (void)out;
  (void)sock;
  (void)make;
  unsupported();
}

int64_t tz_udp_local_port(void *sock, TzMaker make) {
  (void)sock;
  (void)make;
  unsupported();
}

void tz_udp_close(void *sock, TzMaker make) {
  (void)sock;
  (void)make;
}

void tz_udp_lookup(TzPromise *p, void *host_obj, uint64_t host_meta, int64_t family, TzMaker make) {
  (void)p;
  (void)host_obj;
  (void)host_meta;
  (void)family;
  (void)make;
  unsupported();
}

#endif
