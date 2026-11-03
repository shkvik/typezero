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

#define SOCKET_RECEIVE_DEFAULT 65536
#define SOCKET_RECEIVE_MAX (16 * 1024 * 1024)

static TzStr socket_ascii(const char *text) { return tz_str_from_ascii(text, strlen(text)); }

static void *socket_failure(TzMaker make, const char *code, const char *message,
                            const char *syscall) {
  TzSystemErrorParts parts;
  memset(&parts, 0, sizeof parts);
  parts.message = net_utf8(message);
  parts.errno_value = -1;
  parts.code = socket_ascii(code);
  parts.syscall = socket_ascii(syscall);

  return make(&parts);
}

#ifdef _WIN32
#include <afunix.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

typedef struct RawSocket {
  int64_t rc;
  const TzLayout *layout;
  SOCKET s;
  int32_t family, type, protocol;
  uint8_t closed, orphan, bound;
  int32_t ops;
  TzMaker make;
  NetOp rop;
  TzPromise *received;
  WSABUF buffer;
  uint8_t *data;
  SOCKADDR_STORAGE from;
  INT from_length;
  DWORD flags;
  char sender[128];
  int32_t sender_port;
  NetOp aop;
  TzPromise *accepted;
  SOCKET accepting;
  uint8_t addresses[2 * (sizeof(SOCKADDR_STORAGE) + 16)];
  NetOp cop;
  TzPromise *connecting;
  char where[160];
  TzLock lock;
  uint8_t pooled, busy;
} RawSocket;

typedef struct RawSend {
  NetOp op;
  RawSocket *r;
  TzPromise *promise;
  TzMaker make;
  char where[160];
  uint32_t size;
  uint8_t data[];
} RawSend;

LPFN_ACCEPTEX tz_socket_accept_ex;
LPFN_CONNECTEX tz_socket_connect_ex;

static void raw_drop(void *object);
static const TzLayout raw_layout = {raw_drop, NULL, 0, 0, "Socket", 6, 0};

static void raw_free(RawSocket *r) {
  tz_free(r->data);
  tz_obj_free(r);
}

static void raw_settle(RawSocket *r) {
  if (r->orphan && r->ops == 0 && !r->busy) {
    raw_free(r);
  }
}

static void raw_enter(RawSocket *r) {
#if TZ_PARALLEL
  if (r->pooled) {
    tz_lock(&r->lock);
    ++tz_thread.conn_locks;
    return;
  }
#else
  (void)r;
#endif
  TZ_ORDERED();
}

static void raw_leave(RawSocket *r) {
#if TZ_PARALLEL
  if (r->pooled) {
    --tz_thread.conn_locks;
    tz_unlock(&r->lock);
  }
#else
  (void)r;
#endif
}

static void raw_close(RawSocket *r) {
  if (r->closed) {
    return;
  }
  r->closed = 1;
  closesocket(r->s);
}

static void forget(TzPromise **p) {
  if (*p) {
    TzPromise *promise = *p;
    *p = NULL;
    tz_obj_release((TzObject *)promise);
  }
}

static void raw_drop(void *object) {
  RawSocket *r = (RawSocket *)object;
  raw_enter(r);
  raw_close(r);
  forget(&r->received);
  forget(&r->accepted);
  forget(&r->connecting);
  r->orphan = 1;
  int dead = r->ops == 0 && !r->busy;
  raw_leave(r);
  if (dead) {
    raw_free(r);
  }
}

#if TZ_PARALLEL
static void socket_completed_pooled(OVERLAPPED_ENTRY *entry);
void *tz_pool_port_handle(void);
void tz_trigger_hold(void);
void tz_io_done_remote(TzWork *w);
#endif

static int raw_attach(SOCKET s, uint8_t *pooled) {
  *pooled = 0;
#if TZ_PARALLEL
  HANDLE pool = (HANDLE)tz_pool_port_handle();
  if (pool) {
    if (!CreateIoCompletionPort((HANDLE)s, pool, (ULONG_PTR)socket_completed_pooled, 0)) {
      return 0;
    }
    SetFileCompletionNotificationModes((HANDLE)s, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS |
                                                      FILE_SKIP_SET_EVENT_ON_HANDLE);
    *pooled = 1;
    return 1;
  }
#endif

  return net_attach(s);
}

static RawSocket *raw_new(SOCKET s, int family, int type, int protocol) {
  RawSocket *r = (RawSocket *)tz_obj_alloc(sizeof(RawSocket));
  memset(r, 0, sizeof *r);
  r->rc = 1;
  r->layout = &raw_layout;
  r->s = s;
  r->family = family;
  r->type = type;
  r->protocol = protocol;
  r->accepting = INVALID_SOCKET;

  return r;
}

static void load_extensions(SOCKET s) {
  DWORD bytes;
  if (!tz_socket_accept_ex) {
    GUID guid = WSAID_ACCEPTEX;
    WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof guid, &tz_socket_accept_ex,
             sizeof tz_socket_accept_ex, &bytes, NULL, NULL);
  }
  if (!tz_socket_connect_ex) {
    GUID guid = WSAID_CONNECTEX;
    WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof guid, &tz_socket_connect_ex,
             sizeof tz_socket_connect_ex, &bytes, NULL, NULL);
  }
}

static int text_of(void *obj, uint64_t meta, char *out, size_t size) {
  TzStr s = {obj, meta};
  uint32_t length;
  int two;
  const void *units = tz_str_units(&s, &length, &two);
  size_t at = 0;
  for (uint32_t i = 0; i < length; ++i) {
    uint32_t c = two ? ((const uint16_t *)units)[i] : ((const uint8_t *)units)[i];
    if (two && c >= 0xD800 && c <= 0xDBFF && i + 1 < length) {
      uint32_t low = ((const uint16_t *)units)[i + 1];
      if (low >= 0xDC00 && low <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
        ++i;
      }
    }
    if (c == 0) {
      return 0;
    }
    char encoded[4];
    size_t n = 0;
    if (c < 0x80) {
      encoded[n++] = (char)c;
    } else if (c < 0x800) {
      encoded[n++] = (char)(0xC0 | (c >> 6));
      encoded[n++] = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      encoded[n++] = (char)(0xE0 | (c >> 12));
      encoded[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
      encoded[n++] = (char)(0x80 | (c & 0x3F));
    } else {
      encoded[n++] = (char)(0xF0 | (c >> 18));
      encoded[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
      encoded[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
      encoded[n++] = (char)(0x80 | (c & 0x3F));
    }
    if (at + n + 1 > size) {
      return 0;
    }
    memcpy(out + at, encoded, n);
    at += n;
  }
  out[at] = 0;

  return 1;
}

static void *target_of(RawSocket *r, void *obj, uint64_t meta, int64_t port, SOCKADDR_STORAGE *out,
                       int *length, char *where, size_t size, TzMaker make, const char *syscall) {
  memset(out, 0, sizeof *out);
  char text[sizeof(((SOCKADDR_UN *)0)->sun_path)];
  if (!text_of(obj, meta, text, sizeof text)) {
    return socket_failure(make, "ERR_INVALID_ADDRESS", "the address is too long or has a zero",
                          syscall);
  }
  if (r->family == AF_UNIX) {
    SOCKADDR_UN *unix_address = (SOCKADDR_UN *)out;
    unix_address->sun_family = AF_UNIX;
    memcpy(unix_address->sun_path, text, strlen(text) + 1);
    *length = (int)sizeof(SOCKADDR_UN);
    snprintf(where, size, "%s", text);
    return NULL;
  }
  if (port < 0 || port > 65535) {
    return socket_failure(make, "ERR_INVALID_ADDRESS", "the port is not within 0 to 65535",
                          syscall);
  }
  if (text[0] == 0) {
    out->ss_family = (ADDRESS_FAMILY)r->family;
    if (r->family == AF_INET6) {
      ((SOCKADDR_IN6 *)out)->sin6_port = htons((u_short)port);
      *length = (int)sizeof(SOCKADDR_IN6);
    } else {
      ((SOCKADDR_IN *)out)->sin_port = htons((u_short)port);
      *length = (int)sizeof(SOCKADDR_IN);
    }
    snprintf(where, size, "%s:%lld", r->family == AF_INET6 ? "[::]" : "0.0.0.0", (long long)port);
    return NULL;
  }
  if (!net_parse_literal(text, (int)port, out, length)) {
    char message[200];
    snprintf(message, sizeof message, "%s is not an IP address", text);
    return socket_failure(make, "ERR_INVALID_ADDRESS", message, syscall);
  }
  if (out->ss_family != r->family) {
    char message[200];
    snprintf(message, sizeof message, "%s is an %s address, and the socket is %s", text,
             out->ss_family == AF_INET ? "IPv4" : "IPv6", r->family == AF_INET ? "IPv4" : "IPv6");
    return socket_failure(make, "ERR_INVALID_ADDRESS", message, syscall);
  }
  snprintf(where, size, out->ss_family == AF_INET6 ? "[%s]:%lld" : "%s:%lld", text,
           (long long)port);

  return NULL;
}

void *tz_socket_open(int64_t domain, int64_t type, int64_t protocol, TzMaker make) {
  TZ_ORDERED();
  net_start();
  tz_net_make = make;
  int family = domain == 1 ? AF_INET : domain == 2 ? AF_INET6 : AF_UNIX;
  int kind = type == 1   ? SOCK_STREAM
             : type == 2 ? SOCK_DGRAM
             : type == 3 ? SOCK_RAW
                         : SOCK_SEQPACKET;
  SOCKET s = WSASocketW(family, kind, (int)protocol, NULL, 0,
                        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  if (s == INVALID_SOCKET) {
    tz_failed = net_system_error(make, WSAGetLastError(), "socket", NULL, 1);
    return NULL;
  }
  uint8_t pooled = 0;
  if (!raw_attach(s, &pooled)) {
    int32_t error = (int32_t)GetLastError();
    closesocket(s);
    tz_failed = net_system_error(make, error, "socket", NULL, 1);
    return NULL;
  }
  if (kind == SOCK_DGRAM) {
    BOOL off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bytes, NULL, NULL);
  }
  RawSocket *r = raw_new(s, family, kind, (int)protocol);
  r->pooled = pooled;

  return r;
}

int64_t tz_socket_bind(void *sock, void *address_obj, uint64_t address_meta, int64_t port,
                       TzMaker make) {
  TZ_ORDERED();
  RawSocket *r = (RawSocket *)sock;
  if (r->closed) {
    tz_failed = socket_failure(make, "ERR_SOCKET_CLOSED", "the socket is closed", "bind");
    return 0;
  }
  SOCKADDR_STORAGE address;
  int length = 0;
  char where[160];
  void *problem = target_of(r, address_obj, address_meta, port, &address, &length, where,
                            sizeof where, make, "bind");
  if (problem) {
    tz_failed = problem;
    return 0;
  }
  if (bind(r->s, (SOCKADDR *)&address, length) == SOCKET_ERROR) {
    tz_failed = net_system_error(make, WSAGetLastError(), "bind", where, 1);
    return 0;
  }
  r->bound = 1;

  return 0;
}

int64_t tz_socket_listen(void *sock, int64_t backlog, TzMaker make) {
  TZ_ORDERED();
  RawSocket *r = (RawSocket *)sock;
  if (r->closed) {
    tz_failed = socket_failure(make, "ERR_SOCKET_CLOSED", "the socket is closed", "listen");
    return 0;
  }
  if (listen(r->s, backlog > 0 ? (int)backlog : SOMAXCONN) == SOCKET_ERROR) {
    tz_failed = net_system_error(make, WSAGetLastError(), "listen", NULL, 1);
    return 0;
  }

  return 0;
}

static void accept_issue(RawSocket *r);
static void connect_issue(RawSocket *r, SOCKADDR_STORAGE *address, int length);
static void send_issue(RawSend *q, SOCKADDR_STORAGE *address, int length, DWORD how);
static void receive_issue(RawSocket *r);
static void accept_settle(RawSocket *r, int32_t error);
static void connect_settle(RawSocket *r, int32_t error);
static void send_settle(RawSend *q, int32_t error, uint32_t sent);
static void receive_settle(RawSocket *r, int32_t error, uint32_t bytes);

enum { LATER_ACCEPT, LATER_CONNECT, LATER_SEND, LATER_RECEIVE };

#if TZ_PARALLEL
void tz_work_ref_count(int32_t n);

typedef struct SocketLater {
  TzAfterPiece base;
  RawSocket *r;
  RawSend *q;
  int32_t kind;
  int32_t length;
  DWORD how;
  SOCKADDR_STORAGE address;
} SocketLater;

static void socket_later_drop(void *object) { tz_obj_free(object); }

static const TzObjectDesc socket_later_desc = {socket_later_drop};

static void socket_later_closed(SocketLater *later) {
  RawSocket *r = later->r;
  switch (later->kind) {
  case LATER_ACCEPT:
    accept_settle(r, ERROR_OPERATION_ABORTED);
    break;
  case LATER_CONNECT:
    connect_settle(r, ERROR_OPERATION_ABORTED);
    break;
  case LATER_SEND:
    send_settle(later->q, ERROR_OPERATION_ABORTED, 0);
    break;
  default:
    receive_settle(r, ERROR_OPERATION_ABORTED, 0);
    break;
  }
}

static void socket_later_orphan(SocketLater *later) {
  RawSocket *r = later->r;
  if (later->kind == LATER_ACCEPT && r->accepting != INVALID_SOCKET) {
    closesocket(r->accepting);
    r->accepting = INVALID_SOCKET;
  }
  if (later->kind == LATER_SEND) {
    forget(&later->q->promise);
    tz_free(later->q);
  }
}

static void socket_later_issue(SocketLater *later) {
  RawSocket *r = later->r;
  switch (later->kind) {
  case LATER_ACCEPT:
    accept_issue(r);
    break;
  case LATER_CONNECT:
    connect_issue(r, &later->address, later->length);
    break;
  case LATER_SEND:
    send_issue(later->q, &later->address, later->length, later->how);
    break;
  default:
    receive_issue(r);
    break;
  }
}

static void socket_later_run(TzAfterPiece *a, int32_t pooled) {
  SocketLater *later = (SocketLater *)a;
  RawSocket *r = later->r;
  raw_enter(r);
  if (pooled) {
    tz_trigger_hold();
  }
  if (r->orphan) {
    socket_later_orphan(later);
  } else if (r->closed) {
    socket_later_closed(later);
  } else {
    socket_later_issue(later);
  }
  --r->ops;
  int dead = r->orphan && r->ops == 0 && !r->busy;
  raw_leave(r);
  if (pooled) {
    tz_ahead_settle_held();
  }
  tz_work_ref_count(-1);
  if (dead) {
    raw_free(r);
  }
}
#endif

static int socket_later(RawSocket *r, int32_t kind, RawSend *q, SOCKADDR_STORAGE *address,
                        int length, DWORD how) {
#if TZ_PARALLEL
  if (!r->pooled) {
    return 0;
  }
  SocketLater *later = (SocketLater *)tz_obj_alloc(sizeof(SocketLater));
  later->r = r;
  later->q = q;
  later->kind = kind;
  later->length = length;
  later->how = how;
  if (address && length > 0) {
    memcpy(&later->address, address, sizeof later->address);
  }
  ++r->ops;
  tz_work_ref_count(1);
  tz_after_piece(&later->base, &socket_later_desc, socket_later_run);

  return 1;
#else
  (void)r;
  (void)kind;
  (void)q;
  (void)address;
  (void)length;
  (void)how;

  return 0;
#endif
}

static void accept_done(TzWork *w);

static void accept_settle(RawSocket *r, int32_t error) {
  SOCKET s = r->accepting;
  r->accepting = INVALID_SOCKET;
  if (error || r->closed) {
    closesocket(s);
    net_reject(&r->accepted, error ? net_system_error(r->make, error, "accept", NULL, 1)
                                   : socket_failure(r->make, "ERR_SOCKET_CLOSED",
                                                    "the socket is closed", "accept"));
    return;
  }
  setsockopt(s, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (const char *)&r->s, sizeof r->s);
  uint8_t pooled = 0;
  if (!raw_attach(s, &pooled)) {
    int32_t attach_error = (int32_t)GetLastError();
    closesocket(s);
    net_reject(&r->accepted, net_system_error(r->make, attach_error, "accept", NULL, 1));
    return;
  }
  RawSocket *peer = raw_new(s, r->family, r->type, r->protocol);
  peer->bound = 1;
  peer->pooled = pooled;
  net_fulfill(&r->accepted, &peer, sizeof peer);
}

static void accept_done(TzWork *w) {
  RawSocket *r = CONTAINING_RECORD(CONTAINING_RECORD(w, NetOp, work), RawSocket, aop);
  --r->ops;
  if (r->orphan) {
    closesocket(r->accepting);
    r->accepting = INVALID_SOCKET;
    raw_settle(r);
    return;
  }
  accept_settle(r, net_op_error(&r->aop, r->s, r->closed));
}

static void accept_start(TzPromise *p, void *sock, TzMaker make) {
  RawSocket *r = (RawSocket *)sock;
  tz_ref_retain(p);
  TzPromise *promise = p;
  r->make = make;
  if (r->closed) {
    net_reject(&promise,
               socket_failure(make, "ERR_SOCKET_CLOSED", "the socket is closed", "accept"));
    return;
  }
  if (r->accepted) {
    net_reject(&promise, socket_failure(make, "ERR_SOCKET_BUSY", "the socket is accepting already",
                                        "accept"));
    return;
  }
  if (r->type != SOCK_STREAM && r->type != SOCK_SEQPACKET) {
    net_reject(&promise, socket_failure(make, "ERR_NOT_SUPPORTED",
                                        "only a stream socket accepts connections", "accept"));
    return;
  }
  load_extensions(r->s);
  SOCKET s = WSASocketW(r->family, r->type, r->protocol, NULL, 0,
                        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  if (s == INVALID_SOCKET || !tz_socket_accept_ex) {
    int32_t error = s == INVALID_SOCKET ? WSAGetLastError() : WSAEOPNOTSUPP;
    if (s != INVALID_SOCKET) {
      closesocket(s);
    }
    net_reject(&promise, net_system_error(make, error, "accept", NULL, 1));
    return;
  }
  r->accepted = promise;
  r->accepting = s;
  if (socket_later(r, LATER_ACCEPT, NULL, NULL, 0, 0)) {
    return;
  }
  accept_issue(r);
}

static void accept_issue(RawSocket *r) {
  SOCKET s = r->accepting;
  DWORD got = 0;
  net_op_begin(&r->aop, accept_done, 0);
  ++r->ops;
  BOOL ok = tz_socket_accept_ex(r->s, s, r->addresses, 0, sizeof(SOCKADDR_STORAGE) + 16,
                                sizeof(SOCKADDR_STORAGE) + 16, &got, &r->aop.ov);
  if (!ok && WSAGetLastError() == ERROR_IO_PENDING) {
    net_op_wait(&r->aop);
    return;
  }
  --r->ops;
  accept_settle(r, ok ? 0 : WSAGetLastError());
}

static void connect_done(TzWork *w);

static void connect_settle(RawSocket *r, int32_t error) {
  if (error) {
    net_reject(&r->connecting, net_system_error(r->make, error, "connect", r->where, 1));
    return;
  }
  setsockopt(r->s, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
  net_fulfill(&r->connecting, NULL, 0);
}

static void connect_done(TzWork *w) {
  RawSocket *r = CONTAINING_RECORD(CONTAINING_RECORD(w, NetOp, work), RawSocket, cop);
  --r->ops;
  if (r->orphan) {
    raw_settle(r);
    return;
  }
  connect_settle(r, net_op_error(&r->cop, r->s, r->closed));
}

static void connect_issue(RawSocket *r, SOCKADDR_STORAGE *address, int length) {
  net_op_begin(&r->cop, connect_done, 0);
  ++r->ops;
  BOOL ok = tz_socket_connect_ex(r->s, (SOCKADDR *)address, length, NULL, 0, NULL, &r->cop.ov);
  if (!ok && WSAGetLastError() == ERROR_IO_PENDING) {
    net_op_wait(&r->cop);
    return;
  }
  --r->ops;
  connect_settle(r, ok ? 0 : WSAGetLastError());
}

static void connect_start(TzPromise *p, void *sock, void *address_obj, uint64_t address_meta,
                          int64_t port, TzMaker make) {
  RawSocket *r = (RawSocket *)sock;
  tz_ref_retain(p);
  TzPromise *promise = p;
  r->make = make;
  if (r->closed) {
    net_reject(&promise,
               socket_failure(make, "ERR_SOCKET_CLOSED", "the socket is closed", "connect"));
    return;
  }
  if (r->connecting) {
    net_reject(&promise, socket_failure(make, "ERR_SOCKET_BUSY", "the socket is connecting already",
                                        "connect"));
    return;
  }
  SOCKADDR_STORAGE address;
  int length = 0;
  void *problem = target_of(r, address_obj, address_meta, port, &address, &length, r->where,
                            sizeof r->where, make, "connect");
  if (problem) {
    net_reject(&promise, problem);
    return;
  }
  if (r->type == SOCK_STREAM && r->family != AF_UNIX) {
    load_extensions(r->s);
    if (!r->bound) {
      SOCKADDR_STORAGE any;
      memset(&any, 0, sizeof any);
      any.ss_family = (ADDRESS_FAMILY)r->family;
      if (bind(r->s, (SOCKADDR *)&any, length) == SOCKET_ERROR) {
        net_reject(&promise, net_system_error(make, WSAGetLastError(), "connect", r->where, 1));
        return;
      }
      r->bound = 1;
    }
    if (!tz_socket_connect_ex) {
      net_reject(&promise, net_system_error(make, WSAEOPNOTSUPP, "connect", r->where, 1));
      return;
    }
    r->connecting = promise;
    if (socket_later(r, LATER_CONNECT, NULL, &address, length, 0)) {
      return;
    }
    connect_issue(r, &address, length);
    return;
  }
  if (connect(r->s, (SOCKADDR *)&address, length) == SOCKET_ERROR) {
    net_reject(&promise, net_system_error(make, WSAGetLastError(), "connect", r->where, 1));
    return;
  }
  r->bound = 1;
  net_fulfill(&promise, NULL, 0);
}

static void send_done(TzWork *w);

static void send_settle(RawSend *q, int32_t error, uint32_t sent) {
  RawSocket *r = q->r;
  if (error) {
    net_reject(&q->promise,
               net_system_error(q->make, error, "send", q->where[0] ? q->where : NULL, 1));
  } else {
    int64_t count = (int64_t)sent;
    net_fulfill(&q->promise, &count, sizeof count);
  }
  tz_free(q);
  raw_settle(r);
}

static void send_done(TzWork *w) {
  RawSend *q = (RawSend *)CONTAINING_RECORD(w, NetOp, work);
  RawSocket *r = q->r;
  --r->ops;
  send_settle(q, net_op_error(&q->op, r->s, r->closed), q->op.bytes);
}

static void send_start(TzPromise *p, void *sock, void *data, void *address_obj,
                       uint64_t address_meta, int64_t port, int64_t flags, TzMaker make) {
  RawSocket *r = (RawSocket *)sock;
  tz_ref_retain(p);
  TzPromise *promise = p;
  if (r->closed) {
    net_reject(&promise, socket_failure(make, "ERR_SOCKET_CLOSED", "the socket is closed", "send"));
    return;
  }
  TzStr address_text = {address_obj, address_meta};
  uint32_t address_length = 0;
  int two = 0;
  tz_str_units(&address_text, &address_length, &two);
  SOCKADDR_STORAGE address;
  int length = 0;
  char where[160];
  where[0] = 0;
  if (address_length > 0) {
    void *problem = target_of(r, address_obj, address_meta, port, &address, &length, where,
                              sizeof where, make, "send");
    if (problem) {
      net_reject(&promise, problem);
      return;
    }
  }
  TzBytes *bytes = (TzBytes *)data;
  size_t size = (size_t)tz_bytes_byte_length(bytes);
  RawSend *q = (RawSend *)tz_alloc(sizeof(RawSend) + size);
  memset(q, 0, sizeof *q);
  q->r = r;
  q->promise = promise;
  q->make = make;
  memcpy(q->where, where, sizeof q->where);
  if (size) {
    memcpy(q->data, bytes->data, size);
  }
  q->size = (uint32_t)size;
  DWORD how = ((flags & 1) ? MSG_OOB : 0) | ((flags & 2) ? MSG_DONTROUTE : 0);
  if (address_length == 0) {
    length = 0;
  }
  if (socket_later(r, LATER_SEND, q, &address, length, how)) {
    return;
  }
  send_issue(q, &address, length, how);
}

static void send_issue(RawSend *q, SOCKADDR_STORAGE *address, int length, DWORD how) {
  RawSocket *r = q->r;
  WSABUF buffer;
  buffer.len = (ULONG)q->size;
  buffer.buf = (CHAR *)q->data;
  DWORD sent = 0;
  net_op_begin(&q->op, send_done, 0);
  ++r->ops;
  int rc = length > 0 ? WSASendTo(r->s, &buffer, 1, &sent, how, (SOCKADDR *)address, length,
                                  &q->op.ov, NULL)
                      : WSASend(r->s, &buffer, 1, &sent, how, &q->op.ov, NULL);
  int32_t error = rc == SOCKET_ERROR ? WSAGetLastError() : 0;
  if (error == WSA_IO_PENDING) {
    net_op_wait(&q->op);
    return;
  }
  --r->ops;
  send_settle(q, error, sent);
}

static void receive_done(TzWork *w);

static void receive_settle(RawSocket *r, int32_t error, uint32_t bytes) {
  if (error == WSAEMSGSIZE || error == ERROR_MORE_DATA) {
    error = 0;
    bytes = r->buffer.len;
  }
  if (error) {
    net_reject(&r->received, net_system_error(r->make, error, "recv", NULL, 1));
    return;
  }
  r->sender[0] = 0;
  r->sender_port = 0;
  if (r->type != SOCK_STREAM && r->from_length > 0) {
    net_address_text((const SOCKADDR *)&r->from, r->sender, sizeof r->sender);
    r->sender_port = net_address_port((const SOCKADDR *)&r->from);
  }
  TzBytes *data = net_bytes(r->data, bytes);
  net_fulfill(&r->received, &data, sizeof data);
}

static void receive_done(TzWork *w) {
  RawSocket *r = CONTAINING_RECORD(CONTAINING_RECORD(w, NetOp, work), RawSocket, rop);
  --r->ops;
  if (r->orphan) {
    raw_settle(r);
    return;
  }
  receive_settle(r, net_op_error(&r->rop, r->s, r->closed), r->rop.bytes);
}

static void receive_start(TzPromise *p, void *sock, int64_t size, int64_t flags, TzMaker make) {
  RawSocket *r = (RawSocket *)sock;
  tz_ref_retain(p);
  TzPromise *promise = p;
  r->make = make;
  if (r->closed) {
    net_reject(&promise, socket_failure(make, "ERR_SOCKET_CLOSED", "the socket is closed", "recv"));
    return;
  }
  if (r->received) {
    net_reject(&promise,
               socket_failure(make, "ERR_SOCKET_BUSY", "the socket is receiving already", "recv"));
    return;
  }
  uint32_t capacity = size <= 0                   ? SOCKET_RECEIVE_DEFAULT
                      : size > SOCKET_RECEIVE_MAX ? SOCKET_RECEIVE_MAX
                                                  : (uint32_t)size;
  tz_free(r->data);
  r->data = (uint8_t *)tz_alloc(capacity);
  r->buffer.len = capacity;
  r->buffer.buf = (CHAR *)r->data;
  r->from_length = sizeof r->from;
  r->flags =
      ((flags & 1) ? MSG_PEEK : 0) | ((flags & 2) ? MSG_OOB : 0) | ((flags & 4) ? MSG_WAITALL : 0);
  r->received = promise;
  if (socket_later(r, LATER_RECEIVE, NULL, NULL, 0, 0)) {
    return;
  }
  receive_issue(r);
}

static void receive_issue(RawSocket *r) {
  DWORD got = 0;
  net_op_begin(&r->rop, receive_done, 0);
  ++r->ops;
  int rc = r->type == SOCK_STREAM
               ? WSARecv(r->s, &r->buffer, 1, &got, &r->flags, &r->rop.ov, NULL)
               : WSARecvFrom(r->s, &r->buffer, 1, &got, &r->flags, (SOCKADDR *)&r->from,
                             &r->from_length, &r->rop.ov, NULL);
  int32_t error = rc == SOCKET_ERROR ? WSAGetLastError() : 0;
  if (error == WSA_IO_PENDING) {
    net_op_wait(&r->rop);
    return;
  }
  --r->ops;
  receive_settle(r, error, got);
}

void tz_socket_sender_address(TzStr *out, void *sock, TzMaker make) {
  (void)make;
  RawSocket *r = (RawSocket *)sock;
  *out = net_utf8(r->sender);
}

int64_t tz_socket_sender_port(void *sock, TzMaker make) {
  (void)make;

  return ((RawSocket *)sock)->sender_port;
}

int64_t tz_socket_shutdown(void *sock, int64_t how, TzMaker make) {
  TZ_ORDERED();
  RawSocket *r = (RawSocket *)sock;
  if (r->closed) {
    tz_failed = socket_failure(make, "ERR_SOCKET_CLOSED", "the socket is closed", "shutdown");
    return 0;
  }
  int side = how == 0 ? SD_RECEIVE : how == 1 ? SD_SEND : SD_BOTH;
  if (shutdown(r->s, side) == SOCKET_ERROR) {
    tz_failed = net_system_error(make, WSAGetLastError(), "shutdown", NULL, 1);
    return 0;
  }

  return 0;
}

static const char *const kOptionNames[] = {
    "",
    "reuseAddress",
    "broadcast",
    "keepAlive",
    "keepAliveDelay",
    "keepAliveInterval",
    "keepAliveCount",
    "noDelay",
    "linger",
    "sendBufferSize",
    "receiveBufferSize",
    "ttl",
    "typeOfService",
    "ipv6Only",
    "headerIncluded",
    "multicastTtl",
    "multicastLoop",
    "userTimeout",
    "outOfBandInline",
    "dontFragment",
};

int64_t tz_socket_option(void *sock, int64_t option, int64_t value, TzMaker make) {
  TZ_ORDERED();
  RawSocket *r = (RawSocket *)sock;
  const char *name = option > 0 && option < (int64_t)(sizeof kOptionNames / sizeof kOptionNames[0])
                         ? kOptionNames[option]
                         : "";
  if (r->closed) {
    tz_failed = socket_failure(make, "ERR_SOCKET_CLOSED", "the socket is closed", "setsockopt");
    return 0;
  }
  int v6 = r->family == AF_INET6;
  DWORD number = value < 0 ? 0 : (DWORD)value;
  DWORD seconds = (DWORD)((value + 999) / 1000);
  if (seconds == 0) {
    seconds = 1;
  }
  struct linger hold = {value >= 0 ? 1 : 0, (u_short)(value >= 0 ? value : 0)};
  int level = SOL_SOCKET, which = 0;
  const char *pointer = (const char *)&number;
  int size = sizeof number;
  switch (option) {
  case 1:
    which = SO_REUSEADDR;
    break;
  case 2:
    which = SO_BROADCAST;
    break;
  case 3:
    which = SO_KEEPALIVE;
    break;
  case 4:
    level = IPPROTO_TCP;
    which = TCP_KEEPIDLE;
    pointer = (const char *)&seconds;
    break;
  case 5:
    level = IPPROTO_TCP;
    which = TCP_KEEPINTVL;
    pointer = (const char *)&seconds;
    break;
  case 6:
    level = IPPROTO_TCP;
    which = TCP_KEEPCNT;
    break;
  case 7:
    level = IPPROTO_TCP;
    which = TCP_NODELAY;
    break;
  case 8:
    which = SO_LINGER;
    pointer = (const char *)&hold;
    size = sizeof hold;
    break;
  case 9:
    which = SO_SNDBUF;
    break;
  case 10:
    which = SO_RCVBUF;
    break;
  case 11:
    level = v6 ? IPPROTO_IPV6 : IPPROTO_IP;
    which = v6 ? IPV6_UNICAST_HOPS : IP_TTL;
    break;
  case 12:
    level = v6 ? IPPROTO_IPV6 : IPPROTO_IP;
    which = v6 ? IPV6_TCLASS : IP_TOS;
    break;
  case 13:
    level = IPPROTO_IPV6;
    which = IPV6_V6ONLY;
    break;
  case 14:
    level = v6 ? IPPROTO_IPV6 : IPPROTO_IP;
    which = v6 ? IPV6_HDRINCL : IP_HDRINCL;
    break;
  case 15:
    level = v6 ? IPPROTO_IPV6 : IPPROTO_IP;
    which = v6 ? IPV6_MULTICAST_HOPS : IP_MULTICAST_TTL;
    break;
  case 16:
    level = v6 ? IPPROTO_IPV6 : IPPROTO_IP;
    which = v6 ? IPV6_MULTICAST_LOOP : IP_MULTICAST_LOOP;
    break;
  case 17:
    level = IPPROTO_TCP;
    which = TCP_MAXRTMS;
    break;
  case 18:
    which = SO_OOBINLINE;
    break;
  case 19:
    level = v6 ? IPPROTO_IPV6 : IPPROTO_IP;
    which = v6 ? IPV6_DONTFRAG : IP_DONTFRAGMENT;
    break;
  default:
    tz_failed = socket_failure(make, "ERR_INVALID_ARG_VALUE", "no such setting", "setsockopt");
    return 0;
  }
  if (setsockopt(r->s, level, which, pointer, size) == SOCKET_ERROR) {
    tz_failed = net_system_error(make, WSAGetLastError(), "setsockopt", name, 1);
    return 0;
  }

  return 0;
}

static void endpoint_of(RawSocket *r, int remote, char *out, size_t size, int32_t *port) {
  SOCKADDR_STORAGE address;
  int length = sizeof address;
  out[0] = 0;
  *port = 0;
  if (r->closed) {
    return;
  }
  int rc = remote ? getpeername(r->s, (SOCKADDR *)&address, &length)
                  : getsockname(r->s, (SOCKADDR *)&address, &length);
  if (rc == SOCKET_ERROR) {
    return;
  }
  net_address_text((const SOCKADDR *)&address, out, size);
  *port = net_address_port((const SOCKADDR *)&address);
}

void tz_socket_local_address(TzStr *out, void *sock, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  char text[128];
  int32_t port;
  endpoint_of((RawSocket *)sock, 0, text, sizeof text, &port);
  *out = net_utf8(text);
}

int64_t tz_socket_local_port(void *sock, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  char text[128];
  int32_t port;
  endpoint_of((RawSocket *)sock, 0, text, sizeof text, &port);

  return port;
}

void tz_socket_remote_address(TzStr *out, void *sock, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  char text[128];
  int32_t port;
  endpoint_of((RawSocket *)sock, 1, text, sizeof text, &port);
  *out = net_utf8(text);
}

int64_t tz_socket_remote_port(void *sock, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  char text[128];
  int32_t port;
  endpoint_of((RawSocket *)sock, 1, text, sizeof text, &port);

  return port;
}

void tz_socket_accept(TzPromise *p, void *sock, TzMaker make) {
  RawSocket *r = (RawSocket *)sock;
  raw_enter(r);
  accept_start(p, sock, make);
  raw_leave(r);
}

void tz_socket_connect(TzPromise *p, void *sock, void *address_obj, uint64_t address_meta,
                       int64_t port, TzMaker make) {
  RawSocket *r = (RawSocket *)sock;
  raw_enter(r);
  connect_start(p, sock, address_obj, address_meta, port, make);
  raw_leave(r);
}

void tz_socket_send(TzPromise *p, void *sock, void *data, void *address_obj, uint64_t address_meta,
                    int64_t port, int64_t flags, TzMaker make) {
  RawSocket *r = (RawSocket *)sock;
  raw_enter(r);
  send_start(p, sock, data, address_obj, address_meta, port, flags, make);
  raw_leave(r);
}

void tz_socket_receive(TzPromise *p, void *sock, int64_t size, int64_t flags, TzMaker make) {
  RawSocket *r = (RawSocket *)sock;
  raw_enter(r);
  receive_start(p, sock, size, flags, make);
  raw_leave(r);
}

#if TZ_PARALLEL
static RawSocket *op_owner(NetOp *op) {
  void (*done)(TzWork *) = op->work.done;
  if (done == receive_done) {
    return CONTAINING_RECORD(op, RawSocket, rop);
  }
  if (done == connect_done) {
    return CONTAINING_RECORD(op, RawSocket, cop);
  }
  if (done == accept_done) {
    return CONTAINING_RECORD(op, RawSocket, aop);
  }

  return ((RawSend *)op)->r;
}

static void socket_completed_pooled(OVERLAPPED_ENTRY *entry) {
  NetOp *op = CONTAINING_RECORD(entry->lpOverlapped, NetOp, ov);
  op->bytes = entry->dwNumberOfBytesTransferred;
  op->error = entry->Internal != 0 ? -1 : 0;
  RawSocket *r = op_owner(op);
  TzWork work = op->work;
  tz_lock(&r->lock);
  ++tz_thread.conn_locks;
  r->busy = 1;
  tz_trigger_hold();
  op->work.done(&op->work);
  r->busy = 0;
  int dead = r->orphan && r->ops == 0;
  --tz_thread.conn_locks;
  tz_unlock(&r->lock);
  tz_ahead_settle_held();
  tz_io_done_remote(&work);
  if (dead) {
    raw_free(r);
  }
}
#endif

void tz_socket_close(void *sock, TzMaker make) {
  (void)make;
  RawSocket *r = (RawSocket *)sock;
  raw_enter(r);
  raw_close(r);
  raw_leave(r);
}

#else

TZ_NORETURN static void unsupported(void) {
  tz_panicf("tz:socket is not supported on this system yet (docs/net.md, section 7)");
}

void *tz_socket_open(int64_t domain, int64_t type, int64_t protocol, TzMaker make) {
  (void)domain;
  (void)type;
  (void)protocol;
  (void)make;
  (void)socket_failure;
  unsupported();
}
int64_t tz_socket_bind(void *sock, void *address_obj, uint64_t address_meta, int64_t port,
                       TzMaker make) {
  (void)sock;
  (void)address_obj;
  (void)address_meta;
  (void)port;
  (void)make;
  unsupported();
}
int64_t tz_socket_listen(void *sock, int64_t backlog, TzMaker make) {
  (void)sock;
  (void)backlog;
  (void)make;
  unsupported();
}
void tz_socket_accept(TzPromise *p, void *sock, TzMaker make) {
  (void)p;
  (void)sock;
  (void)make;
  unsupported();
}
void tz_socket_connect(TzPromise *p, void *sock, void *address_obj, uint64_t address_meta,
                       int64_t port, TzMaker make) {
  (void)p;
  (void)sock;
  (void)address_obj;
  (void)address_meta;
  (void)port;
  (void)make;
  unsupported();
}
void tz_socket_send(TzPromise *p, void *sock, void *data, void *address_obj, uint64_t address_meta,
                    int64_t port, int64_t flags, TzMaker make) {
  (void)p;
  (void)sock;
  (void)data;
  (void)address_obj;
  (void)address_meta;
  (void)port;
  (void)flags;
  (void)make;
  unsupported();
}
void tz_socket_receive(TzPromise *p, void *sock, int64_t size, int64_t flags, TzMaker make) {
  (void)p;
  (void)sock;
  (void)size;
  (void)flags;
  (void)make;
  unsupported();
}
void tz_socket_sender_address(TzStr *out, void *sock, TzMaker make) {
  (void)out;
  (void)sock;
  (void)make;
  unsupported();
}
int64_t tz_socket_sender_port(void *sock, TzMaker make) {
  (void)sock;
  (void)make;
  unsupported();
}
int64_t tz_socket_shutdown(void *sock, int64_t how, TzMaker make) {
  (void)sock;
  (void)how;
  (void)make;
  unsupported();
}
int64_t tz_socket_option(void *sock, int64_t option, int64_t value, TzMaker make) {
  (void)sock;
  (void)option;
  (void)value;
  (void)make;
  unsupported();
}
void tz_socket_local_address(TzStr *out, void *sock, TzMaker make) {
  (void)out;
  (void)sock;
  (void)make;
  unsupported();
}
int64_t tz_socket_local_port(void *sock, TzMaker make) {
  (void)sock;
  (void)make;
  unsupported();
}
void tz_socket_remote_address(TzStr *out, void *sock, TzMaker make) {
  (void)out;
  (void)sock;
  (void)make;
  unsupported();
}
int64_t tz_socket_remote_port(void *sock, TzMaker make) {
  (void)sock;
  (void)make;
  unsupported();
}
void tz_socket_close(void *sock, TzMaker make) {
  (void)sock;
  (void)make;
  unsupported();
}

#endif
