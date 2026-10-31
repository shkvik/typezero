#include "runtime/library/net/net/net.h"
#include "runtime/library/http/http2/http2.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/async/async.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/io/io.h"
#include "runtime/core/output/output.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include "runtime/library/net/deadline/deadline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

TzMaker tz_net_make;
int32_t tz_net_started;
#ifdef _WIN32
LPFN_ACCEPTEX tz_net_accept_ex;
LPFN_CONNECTEX tz_net_connect_ex;
LPFN_GETACCEPTEXSOCKADDRS tz_net_accept_addresses;
#endif

#define NET_INPUT 16384
#define NET_RAW_MAX (256 * 1024)

static const NetError kNetErrors[] = {
    {"UNKNOWN", "unknown error", -4094},
    {"ECONNREFUSED", "connection refused", -4078},
    {"ECONNRESET", "connection reset by peer", -4077},
    {"ECONNABORTED", "software caused connection abort", -4079},
    {"ETIMEDOUT", "connection timed out", -4039},
    {"EADDRINUSE", "address already in use", -4091},
    {"EADDRNOTAVAIL", "address not available", -4090},
    {"EACCES", "permission denied", -4092},
    {"ENETUNREACH", "network is unreachable", -4062},
    {"EHOSTUNREACH", "host is unreachable", -4065},
    {"ENOTCONN", "socket is not connected", -4053},
    {"EPIPE", "broken pipe", -4047},
    {"ECANCELED", "operation canceled", -4081},
    {"EMFILE", "too many open files", -4066},
    {"ENOBUFS", "no buffer space available", -4060},
    {"EAGAIN", "resource temporarily unavailable", -4088},
    {"EAFNOSUPPORT", "address family not supported", -4089},
    {"EINVAL", "invalid argument", -4071},
    {"ENOTFOUND", "getaddrinfo ENOTFOUND", -3008},
    {"EAI_AGAIN", "temporary failure", -3001},
    {"EMSGSIZE", "message too long", -4040},
};

enum {
  NE_UNKNOWN,
  NE_ECONNREFUSED,
  NE_ECONNRESET,
  NE_ECONNABORTED,
  NE_ETIMEDOUT,
  NE_EADDRINUSE,
  NE_EADDRNOTAVAIL,
  NE_EACCES,
  NE_ENETUNREACH,
  NE_EHOSTUNREACH,
  NE_ENOTCONN,
  NE_EPIPE,
  NE_ECANCELED,
  NE_EMFILE,
  NE_ENOBUFS,
  NE_EAGAIN,
  NE_EAFNOSUPPORT,
  NE_EINVAL,
  NE_ENOTFOUND,
  NE_EAI_AGAIN,
  NE_EMSGSIZE,
};

#define NET_NOT_FOUND (-3008)
#define NET_TRY_AGAIN (-3001)

const NetError *net_error_of(int32_t error) {
  int index = NE_UNKNOWN;
#ifdef _WIN32
  switch (error) {
  case WSAECONNREFUSED:
  case ERROR_CONNECTION_REFUSED:
    index = NE_ECONNREFUSED;
    break;
  case WSAECONNRESET:
  case ERROR_NETNAME_DELETED:
    index = NE_ECONNRESET;
    break;
  case WSAECONNABORTED:
  case ERROR_CONNECTION_ABORTED:
    index = NE_ECONNABORTED;
    break;
  case WSAETIMEDOUT:
  case ERROR_SEM_TIMEOUT:
    index = NE_ETIMEDOUT;
    break;
  case WSAEADDRINUSE:
    index = NE_EADDRINUSE;
    break;
  case WSAEADDRNOTAVAIL:
    index = NE_EADDRNOTAVAIL;
    break;
  case WSAEACCES:
  case ERROR_ACCESS_DENIED:
    index = NE_EACCES;
    break;
  case WSAENETUNREACH:
  case ERROR_NETWORK_UNREACHABLE:
    index = NE_ENETUNREACH;
    break;
  case WSAEHOSTUNREACH:
  case ERROR_HOST_UNREACHABLE:
    index = NE_EHOSTUNREACH;
    break;
  case WSAENOTCONN:
    index = NE_ENOTCONN;
    break;
  case WSAESHUTDOWN:
  case ERROR_BROKEN_PIPE:
    index = NE_EPIPE;
    break;
  case ERROR_OPERATION_ABORTED:
    index = NE_ECANCELED;
    break;
  case WSAEMFILE:
    index = NE_EMFILE;
    break;
  case WSAENOBUFS:
    index = NE_ENOBUFS;
    break;
  case WSAEWOULDBLOCK:
    index = NE_EAGAIN;
    break;
  case WSAEAFNOSUPPORT:
    index = NE_EAFNOSUPPORT;
    break;
  case WSAEINVAL:
    index = NE_EINVAL;
    break;
  case WSAEMSGSIZE:
  case ERROR_MORE_DATA:
    index = NE_EMSGSIZE;
    break;
  case NET_NOT_FOUND:
    index = NE_ENOTFOUND;
    break;
  case NET_TRY_AGAIN:
    index = NE_EAI_AGAIN;
    break;
  default:
    break;
  }
#else
  (void)error;
#endif

  return &kNetErrors[index];
}

static TzStr ascii(const char *text) { return tz_str_from_ascii(text, strlen(text)); }

void *net_system_error(TzMaker make, int32_t error, const char *syscall, const char *where,
                       int with_text) {
  const NetError *e = net_error_of(error);
  char message[512];
  if (error == NET_NOT_FOUND || error == NET_TRY_AGAIN) {
    snprintf(message, sizeof message, "%s %s%s%s", syscall, e->code, where ? " " : "",
             where ? where : "");
  } else if (with_text) {
    snprintf(message, sizeof message, "%s %s: %s%s%s", syscall, e->code, e->message,
             where ? " " : "", where ? where : "");
  } else {
    snprintf(message, sizeof message, "%s %s%s%s", syscall, e->code, where ? " " : "",
             where ? where : "");
  }
  TzSystemErrorParts parts;
  memset(&parts, 0, sizeof parts);
  parts.message = ascii(message);
  parts.errno_value = e->number;
  parts.code = ascii(e->code);
  parts.syscall = ascii(syscall);

  return make(&parts);
}

void *net_failure(const char *code, const char *message) {
  TzSystemErrorParts parts;
  memset(&parts, 0, sizeof parts);
  parts.message = net_utf8(message);
  parts.errno_value = -1;
  parts.code = ascii(code);
  parts.syscall = ascii("");

  return tz_net_make(&parts);
}

void *net_timed_out(void) {
  return net_failure("TIMEOUT_ERR", "The operation was aborted due to timeout");
}

TzStr net_utf8(const char *message) {
  TzBuf b;
  tz_buf_init(&b);
  const unsigned char *s = (const unsigned char *)message;
  for (size_t i = 0; s[i];) {
    uint32_t c = s[i];
    size_t extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra) {
      c &= 0x3Fu >> extra;
    }
    for (size_t k = 1; k <= extra && s[i + k]; ++k) {
      c = (c << 6) | (s[i + k] & 0x3Fu);
    }
    i += extra + 1;
    if (c >= 0x10000) {
      c -= 0x10000;
      tz_buf_push(&b, (uint16_t)(0xD800 + (c >> 10)));
      tz_buf_push(&b, (uint16_t)(0xDC00 + (c & 0x3FF)));
    } else {
      tz_buf_push(&b, (uint16_t)c);
    }
  }

  return tz_buf_take(&b);
}

void net_fulfill(TzPromise **p, const void *value, size_t size) {
  TzPromise *promise = *p;
  *p = NULL;
  tz_promise_succeed(promise, value, size);
  tz_obj_release((TzObject *)promise);
}

void net_reject(TzPromise **p, void *error) {
  TzPromise *promise = *p;
  *p = NULL;
  tz_promise_fail(promise, error);
  tz_obj_release((TzObject *)promise);
}

TzBytes *tz_bytes_new_unset(int32_t kind, int64_t length);

TzBytes *net_bytes(const uint8_t *data, size_t length) {
  TzBytes *b = tz_bytes_new_unset(TZ_BYTES_U8, (int64_t)length);
  if (length) {
    memcpy(b->data, data, length);
  }

  return b;
}

double net_now(void) {
#ifdef _WIN32
  return (double)GetTickCount64();
#else

  return 0;
#endif
}

void tz_net_no_tls(TzPromise *p, TzMaker make) {
  TZ_ORDERED();
  tz_net_make = make;
  void *error =
      net_failure("ERR_NO_TLS", "this program has no TLS for https: tz links it only into programs "
                                "whose fetch may reach https://, and this one names only http:// "
                                "URLs; write the URL with https:// (docs/net.md, section 4)");
  if (p) {
    tz_promise_fail(p, error);
  } else {
    tz_failed = error;
  }
}

#ifdef _WIN32

static void conn_drop(void *object);
static void listener_drop(void *object);
static const TzLayout conn_layout = {conn_drop, NULL, 0, 0, "Socket", 6, 0};
static const TzLayout listener_layout = {listener_drop, NULL, 0, 0, "Server", 6, 0};

static void start(void) {
  if (tz_net_started) {
    return;
  }
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    tz_panicf("cannot start Winsock (error %d)", WSAGetLastError());
  }
  tz_net_started = 1;
}

static void load_extensions(SOCKET s) {
  if (tz_net_accept_ex) {
    return;
  }
  GUID accept_guid = WSAID_ACCEPTEX, connect_guid = WSAID_CONNECTEX,
       addresses_guid = WSAID_GETACCEPTEXSOCKADDRS;
  DWORD bytes;
  WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &accept_guid, sizeof accept_guid,
           &tz_net_accept_ex, sizeof tz_net_accept_ex, &bytes, NULL, NULL);
  WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &connect_guid, sizeof connect_guid,
           &tz_net_connect_ex, sizeof tz_net_connect_ex, &bytes, NULL, NULL);
  WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &addresses_guid, sizeof addresses_guid,
           &tz_net_accept_addresses, sizeof tz_net_accept_addresses, &bytes, NULL, NULL);
}

static void net_completed(OVERLAPPED_ENTRY *entry) {
  NetOp *op = CONTAINING_RECORD(entry->lpOverlapped, NetOp, ov);
  op->bytes = entry->dwNumberOfBytesTransferred;
  op->error = entry->Internal != 0 ? -1 : 0;
  tz_io_due(&op->work);
}

static int attach_to(SOCKET s, HANDLE port, void (*taken)(OVERLAPPED_ENTRY *)) {
  if (!CreateIoCompletionPort((HANDLE)s, port, (ULONG_PTR)taken, 0)) {
    return 0;
  }
  SetFileCompletionNotificationModes((HANDLE)s, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS |
                                                    FILE_SKIP_SET_EVENT_ON_HANDLE);

  return 1;
}

static int attach(SOCKET s) { return attach_to(s, (HANDLE)tz_io_port_handle(), net_completed); }

static void op_begin(NetOp *op, void (*done)(TzWork *), int unref) {
  memset(&op->ov, 0, sizeof op->ov);
  op->work.done = done;
  op->work.work = NULL;
  op->work.turns = 1;
  op->work.unref = (uint32_t)unref;
  op->bytes = 0;
  op->error = 0;
  op->active = 1;
}

static void op_wait(NetOp *op) { tz_io_begin(&op->work); }

static int32_t op_error(NetOp *op, SOCKET s, int closed) {
  if (!op->error) {
    return 0;
  }
  if (closed) {
    return ERROR_OPERATION_ABORTED;
  }
  DWORD bytes = 0, flags = 0;
  if (WSAGetOverlappedResult(s, &op->ov, &bytes, FALSE, &flags)) {
    return 0;
  }
  int32_t error = WSAGetLastError();

  return error ? error : WSAECONNRESET;
}

static NetConn *conn_new(SOCKET s) {
  NetConn *c = (NetConn *)tz_obj_alloc(sizeof(NetConn));
  memset(c, 0, sizeof *c);
  c->rc = 1;
  c->layout = &conn_layout;
  c->s = s;
  c->last_active = net_now();
  BOOL yes = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof yes);

  return c;
}

static void conn_free(NetConn *c) {
  NetConn *parent = c->h2parent;
  if (parent) {
    tz_free(c->in);
    tz_free(c->out);
    tz_obj_free(c);
    tz_instance_release((TzInstance *)parent);
    return;
  }
  NetListener *l = c->listener;
  if (l) {
    if (c->prev) {
      c->prev->next = c->next;
    } else {
      l->conns = c->next;
    }
    if (c->next) {
      c->next->prev = c->prev;
    }
    if (l->closed && !l->conns && l->drained) {
      net_fulfill(&l->drained, NULL, 0);
    }
  }
  if (c->tls) {
    c->tls_ops->release(c);
  }
  tz_free(c->in);
  tz_free(c->out);
  tz_free(c->flight);
  tz_free(c->raw);
  tz_free(c->sealed);
  tz_obj_free(c);
}

static int has_output(const NetConn *c) { return c->out_len || c->sealed_len; }

void tz_http_release(NetConn *c);

static void close_socket(NetConn *c) {
  if (c->closed) {
    return;
  }
  c->closed = 1;
  if (c->h2parent) {
    if (c->server) {
      tz_h2_reset(c);
    } else {
      tz_h2_cancel(c);
    }
    return;
  }
  closesocket(c->s);
}

static void conn_settle(NetConn *c) {
  if (c->orphan && c->ops == 0) {
    conn_free(c);
  }
}

static void conn_unlock_settle(NetConn *c) {
  int gone = c->orphan && c->ops == 0;
  conn_unlock(c);
  if (gone) {
    conn_free(c);
  }
}

static void conn_drop(void *object) {
  NetConn *c = (NetConn *)object;
  if (!c->h2parent) {
    TZ_ORDERED();
  }
  conn_lock(c);
  tz_http_release(c);
  if (c->drain) {
    TzPromise *p = c->drain;
    c->drain = NULL;
    tz_obj_release((TzObject *)p);
  }
  c->orphan = 1;
  if (c->closing && (c->writing || has_output(c))) {
    if (!c->writing) {
      net_flush(c);
    }
  } else {
    close_socket(c);
  }
  conn_unlock_settle(c);
}

static void read_done(TzWork *w);

void net_input_room(NetConn *c, uint32_t room) {
  if (c->in_start == c->in_end) {
    c->in_start = c->in_end = 0;
  }
  if (c->in_cap - c->in_end >= room) {
    return;
  }
  if (c->in_start > 0) {
    memmove(c->in, c->in + c->in_start, c->in_end - c->in_start);
    c->in_end -= c->in_start;
    c->in_start = 0;
  }
  if (c->in_cap - c->in_end < room) {
    uint32_t cap = c->in_cap ? c->in_cap * 2 : NET_INPUT;
    while (cap - c->in_end < room) {
      cap *= 2;
    }
    uint8_t *in = (uint8_t *)tz_alloc(cap);
    if (c->in_end) {
      memcpy(in, c->in, c->in_end);
    }
    tz_free(c->in);
    c->in = in;
    c->in_cap = cap;
  }
}

static void raw_room(NetConn *c) {
  if (c->raw_start == c->raw_end) {
    c->raw_start = c->raw_end = 0;
  }
  uint32_t cap = c->raw_cap ? c->raw_cap : NET_INPUT;
  if (c->raw_full && cap < NET_RAW_MAX) {
    cap *= 2;
  }
  c->raw_full = 0;
  if (cap == c->raw_cap && c->raw_cap - c->raw_end >= 4096) {
    return;
  }
  uint32_t kept = c->raw_end - c->raw_start;
  if (cap == c->raw_cap && c->raw_start > 0) {
    memmove(c->raw, c->raw + c->raw_start, kept);
  } else {
    uint8_t *raw = (uint8_t *)tz_alloc(cap);
    if (kept) {
      memcpy(raw, c->raw + c->raw_start, kept);
    }
    tz_free(c->raw);
    c->raw = raw;
    c->raw_cap = cap;
  }
  c->raw_start = 0;
  c->raw_end = kept;
}

static void raw_got(NetConn *c, uint32_t got) {
  if (got == 0) {
    c->raw_eof = 1;
    return;
  }
  c->raw_full = got == c->raw_cap - c->raw_end;
  c->raw_end += got;
}

void net_read(NetConn *c) {
  if (c->h2parent) {
    if (c->eof || c->error || c->closed) {
      c->on_input(c);
    }
    return;
  }
  if (c->reading || c->closed) {
    return;
  }
  for (;;) {
    if (c->eof || c->error) {
      c->on_input(c);
      return;
    }
    if (c->tls) {
      int ready = c->tls_ops->open(c);
      net_flush(c);
      if (ready || c->error) {
        c->on_input(c);
        return;
      }
      if (c->closed) {
        return;
      }
      raw_room(c);
    } else {
      net_input_room(c, 4096);
    }
    WSABUF buf;
    if (c->tls) {
      buf.len = c->raw_cap - c->raw_end;
      buf.buf = (char *)(c->raw + c->raw_end);
    } else {
      buf.len = c->in_cap - c->in_end;
      buf.buf = (char *)(c->in + c->in_end);
    }
    DWORD got = 0, flags = 0;
    op_begin(&c->rop, read_done, 0);
    c->reading = 1;
    ++c->ops;
    int rc = WSARecv(c->s, &buf, 1, &got, &flags, &c->rop.ov, NULL);
    if (rc == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING) {
      op_wait(&c->rop);
      return;
    }
    --c->ops;
    c->reading = 0;
    if (rc == SOCKET_ERROR) {
      c->error = WSAGetLastError();
      continue;
    }
    c->last_active = net_now();
    if (c->tls) {
      raw_got(c, got);
      continue;
    }
    if (got == 0) {
      c->eof = 1;
    }
    c->in_end += got;
    c->on_input(c);
    return;
  }
}

static void read_done(TzWork *w) {
  NetConn *c = CONTAINING_RECORD(CONTAINING_RECORD(w, NetOp, work), NetConn, rop);
  conn_lock(c);
  --c->ops;
  c->reading = 0;
  if (c->orphan) {
    conn_unlock_settle(c);
    return;
  }
  int32_t error = op_error(&c->rop, c->s, c->closed);
  if (!error && c->closed) {
    error = ERROR_OPERATION_ABORTED;
  }
  if (error) {
    c->error = error;
  } else if (c->tls) {
    c->last_active = net_now();
    raw_got(c, c->rop.bytes);
  } else {
    c->last_active = net_now();
    if (c->rop.bytes == 0) {
      c->eof = 1;
    }
    c->in_end += c->rop.bytes;
  }
  tz_rc_inc(&c->rc);
  if (c->tls && !c->error) {
    net_read(c);
  } else {
    c->on_input(c);
  }
  conn_unlock(c);
  tz_instance_release((TzInstance *)c);
}

uint8_t *net_reserve(NetConn *c, size_t length) {
  if (c->out_len + length > c->out_cap) {
    size_t cap = c->out_cap + c->out_cap / 2 + 4096;
    if (cap < c->out_len + length) {
      cap = c->out_len + length;
    }
    uint8_t *out = (uint8_t *)tz_alloc(cap);
    if (c->out_len) {
      memcpy(out, c->out, c->out_len);
    }
    tz_free(c->out);
    c->out = out;
    c->out_cap = (uint32_t)cap;
  }

  return c->out + c->out_len;
}

void net_wrote(NetConn *c, size_t length) { c->out_len += (uint32_t)length; }

void net_write(NetConn *c, const void *data, size_t length) {
  if (c->closed || c->error || !length) {
    return;
  }
  memcpy(net_reserve(c, length), data, length);
  c->out_len += (uint32_t)length;
}

uint8_t *net_seal_reserve(NetConn *c, size_t length) {
  if (c->sealed_len + length > c->sealed_cap) {
    size_t cap = c->sealed_cap + c->sealed_cap / 2 + 4096;
    if (cap < c->sealed_len + length) {
      cap = c->sealed_len + length;
    }
    uint8_t *sealed = (uint8_t *)tz_alloc(cap);
    if (c->sealed_len) {
      memcpy(sealed, c->sealed, c->sealed_len);
    }
    tz_free(c->sealed);
    c->sealed = sealed;
    c->sealed_cap = (uint32_t)cap;
  }

  return c->sealed + c->sealed_len;
}

size_t net_pending_output(const NetConn *c) {
  if (c->h2parent) {
    return (size_t)c->out_len - c->flight_len;
  }

  return (size_t)c->out_len + c->flight_len + c->sealed_len;
}

static void write_done(TzWork *w);

static void drained(NetConn *c) {
  if (c->drain && (net_pending_output(c) < NET_HIGH_WATER_MARK || c->closed || c->error)) {
    net_fulfill(&c->drain, NULL, 0);
  }
}

static void finish_close(NetConn *c) {
  if (c->closing && !c->writing && !has_output(c)) {
    shutdown(c->s, SD_SEND);
    close_socket(c);
  }
}

void net_flush(NetConn *c) {
  if (c->h2parent) {
    tz_h2_flush(c);
    return;
  }
  while (!c->writing && !c->closed && !c->error) {
    if (c->tls) {
      c->tls_ops->seal(c);
      if (c->error) {
        break;
      }
    }
    uint8_t **queue = c->tls ? &c->sealed : &c->out;
    uint32_t *queued = c->tls ? &c->sealed_len : &c->out_len;
    uint32_t *capacity = c->tls ? &c->sealed_cap : &c->out_cap;
    if (!*queued) {
      break;
    }
    uint8_t *data = *queue;
    uint32_t cap = *capacity;
    *queue = c->flight;
    *capacity = c->flight_cap;
    c->flight = data;
    c->flight_cap = cap;
    c->flight_len = *queued;
    *queued = 0;
    WSABUF buf = {c->flight_len, (char *)c->flight};
    DWORD sent = 0;
    op_begin(&c->wop, write_done, 0);
    c->writing = 1;
    ++c->ops;
    int rc = WSASend(c->s, &buf, 1, &sent, 0, &c->wop.ov, NULL);
    if (rc == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING) {
      op_wait(&c->wop);
      return;
    }
    --c->ops;
    c->writing = 0;
    c->flight_len = 0;
    if (rc == SOCKET_ERROR) {
      c->error = WSAGetLastError();
    }
    c->last_active = net_now();
  }
  if (c->error) {
    c->out_len = 0;
    c->sealed_len = 0;
    if (c->closing) {
      close_socket(c);
    }
  }
  drained(c);
  finish_close(c);
}

static void write_done(TzWork *w) {
  NetConn *c = CONTAINING_RECORD(CONTAINING_RECORD(w, NetOp, work), NetConn, wop);
  conn_lock(c);
  --c->ops;
  c->writing = 0;
  c->flight_len = 0;
  int32_t error = op_error(&c->wop, c->s, c->closed);
  if (error && !c->error) {
    c->error = error;
  }
  c->last_active = net_now();
  if (c->orphan && (c->error || c->closed || !c->closing)) {
    close_socket(c);
    conn_unlock_settle(c);
    return;
  }
  tz_rc_inc(&c->rc);
  net_flush(c);
  if (c->orphan && !c->writing) {
    close_socket(c);
  }
  conn_unlock(c);
  tz_instance_release((TzInstance *)c);
}

#if TZ_PARALLEL
void *tz_pool_port_handle(void);
void tz_trigger_hold(void);
int32_t tz_trigger_take(TzPromise **out);
int32_t tz_ahead_here(TzPromise *p);
void tz_ahead_forward(TzPromise *p);
void tz_io_due_remote(TzWork *w);
void tz_io_done_remote(TzWork *w);

static void net_completed_pooled(OVERLAPPED_ENTRY *entry) {
  NetOp *op = CONTAINING_RECORD(entry->lpOverlapped, NetOp, ov);
  op->bytes = entry->dwNumberOfBytesTransferred;
  op->error = entry->Internal != 0 ? -1 : 0;
  int reading = op->work.done == read_done;
  NetConn *c = reading ? CONTAINING_RECORD(op, NetConn, rop) : CONTAINING_RECORD(op, NetConn, wop);
  conn_lock(c);
  int here =
      !c->orphan && !c->closed && !c->tls && !c->error && !op->error &&
      (reading ? op->bytes > 0 && (tz_http_waits_request(c) || c->h2) : !c->closing && !c->drain);
  if (!here) {
    conn_unlock(c);
    tz_io_due_remote(&op->work);
    return;
  }
  tz_trigger_hold();
  if (reading) {
    --c->ops;
    c->reading = 0;
    c->last_active = net_now();
    c->in_end += op->bytes;
    c->on_input(c);
  } else {
    --c->ops;
    c->writing = 0;
    c->flight_len = 0;
    c->last_active = net_now();
    net_flush(c);
  }
  conn_unlock(c);
  TzPromise *held[16];
  int32_t n = tz_trigger_take(held);
  for (int32_t i = 0; i < n; ++i) {
    if (n > 1 || !tz_ahead_here(held[i])) {
      tz_ahead_forward(held[i]);
    }
  }
  tz_io_done_remote(&op->work);
}
#endif

void net_close(NetConn *c) {
  if (c->closed) {
    return;
  }
  if (c->h2parent) {
    c->closing = 1;
    if (c->server) {
      tz_h2_end(c);
    } else if (!c->eof) {
      tz_h2_cancel(c);
    }
    return;
  }
  c->closing = 1;
  if (c->tls) {
    c->tls_ops->shutdown(c);
  }
  if (c->writing || has_output(c)) {
    net_flush(c);
  } else {
    finish_close(c);
  }
}

void net_abort(NetConn *c) {
  c->out_len = 0;
  c->sealed_len = 0;
  close_socket(c);
}

NetConn *net_stream_new(NetConn *parent) {
  NetConn *c = (NetConn *)tz_obj_alloc(sizeof(NetConn));
  memset(c, 0, sizeof *c);
  c->rc = 1;
  c->layout = &conn_layout;
  c->s = INVALID_SOCKET;
  c->h2parent = parent;
  tz_rc_inc(&parent->rc);
  c->server = 1;
  c->last_active = net_now();
  memcpy(c->peer, parent->peer, sizeof c->peer);
  c->peer_port = parent->peer_port;
  c->http.max_body = parent->http.max_body;
  c->http.request_timeout = parent->http.request_timeout;
  c->on_input = tz_http_stream_input;

  return c;
}

void net_accept_ready(NetListener *l, NetConn *c) {
  tz_lock(&l->ready_lock);
  if (l->pending) {
    net_fulfill(&l->pending, &c, sizeof c);
    tz_unlock(&l->ready_lock);
    return;
  }
  c->ready_next = NULL;
  if (l->ready_last) {
    l->ready_last->ready_next = c;
  } else {
    l->ready_first = c;
  }
  l->ready_last = c;
  tz_unlock(&l->ready_lock);
}

static NetConn *take_ready(NetListener *l) {
  NetConn *c = l->ready_first;
  if (c) {
    l->ready_first = c->ready_next;
    if (!l->ready_first) {
      l->ready_last = NULL;
    }
    c->ready_next = NULL;
  }

  return c;
}

void tz_net_close(void *conn, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  net_close(c);
  conn_unlock(c);
}

static void address_text(const SOCKADDR *sa, char *out, size_t size) {
  out[0] = 0;
  if (sa->sa_family == AF_INET) {
    inet_ntop(AF_INET, &((const SOCKADDR_IN *)sa)->sin_addr, out, size);
  } else if (sa->sa_family == AF_INET6) {
    inet_ntop(AF_INET6, &((const SOCKADDR_IN6 *)sa)->sin6_addr, out, size);
  }
}

static int address_port(const SOCKADDR *sa) {
  return ntohs(sa->sa_family == AF_INET ? ((const SOCKADDR_IN *)sa)->sin_port
                                        : ((const SOCKADDR_IN6 *)sa)->sin6_port);
}

static int ascii_of(void *obj, uint64_t meta, char *out, size_t size) {
  TzStr s = {obj, meta};
  uint32_t length;
  int two;
  const void *units = tz_str_units(&s, &length, &two);
  if (length + 1 > size) {
    return 0;
  }
  for (uint32_t i = 0; i < length; ++i) {
    uint32_t c = two ? ((const uint16_t *)units)[i] : ((const uint8_t *)units)[i];
    if (c == 0 || c >= 0x80) {
      return 0;
    }
    out[i] = (char)c;
  }
  out[length] = 0;

  return 1;
}

static int parse_literal(const char *host, int port, SOCKADDR_STORAGE *out, int *length) {
  memset(out, 0, sizeof *out);
  char text[256];
  size_t n = strlen(host);
  if (n >= sizeof text) {
    return 0;
  }
  if (n >= 2 && host[0] == '[' && host[n - 1] == ']') {
    memcpy(text, host + 1, n - 2);
    text[n - 2] = 0;
  } else {
    memcpy(text, host, n + 1);
  }
  SOCKADDR_IN *v4 = (SOCKADDR_IN *)out;
  if (inet_pton(AF_INET, text, &v4->sin_addr) == 1) {
    v4->sin_family = AF_INET;
    v4->sin_port = htons((u_short)port);
    *length = sizeof(SOCKADDR_IN);
    return 1;
  }
  SOCKADDR_IN6 *v6 = (SOCKADDR_IN6 *)out;
  if (inet_pton(AF_INET6, text, &v6->sin6_addr) == 1) {
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons((u_short)port);
    *length = sizeof(SOCKADDR_IN6);
    return 1;
  }

  return 0;
}

void net_start(void) { start(); }

int net_attach(SOCKET s) { return attach(s); }

void net_op_begin(NetOp *op, void (*done)(TzWork *), int unref) { op_begin(op, done, unref); }

void net_op_wait(NetOp *op) { op_wait(op); }

int32_t net_op_error(NetOp *op, SOCKET s, int closed) { return op_error(op, s, closed); }

void net_address_text(const SOCKADDR *sa, char *out, size_t size) { address_text(sa, out, size); }

int net_address_port(const SOCKADDR *sa) { return address_port(sa); }

int net_ascii_of(void *obj, uint64_t meta, char *out, size_t size) {
  return ascii_of(obj, meta, out, size);
}

int net_parse_literal(const char *host, int port, SOCKADDR_STORAGE *out, int *length) {
  return parse_literal(host, port, out, length);
}

static void listener_free(NetListener *l) {
  for (NetConn *c = l->conns; c; c = c->next) {
    c->listener = NULL;
  }
  if (l->tls) {
    l->tls_release(l->tls);
  }
  tz_obj_free(l);
}

static void listener_close(NetListener *l) {
  if (l->closed) {
    return;
  }
  l->closed = 1;
  if (l->sweep) {
    tz_timer_clear(l->sweep);
    tz_obj_release((TzObject *)l->sweep);
    l->sweep = NULL;
  }
  closesocket(l->s);
  if (l->accepting != INVALID_SOCKET && !l->ops) {
    closesocket(l->accepting);
    l->accepting = INVALID_SOCKET;
  }
}

static void listener_drop(void *object) {
  TZ_ORDERED();
  NetListener *l = (NetListener *)object;
  listener_close(l);
  for (NetConn *c = take_ready(l); c; c = take_ready(l)) {
    tz_instance_release((TzInstance *)c);
  }
  if (l->pending) {
    TzPromise *p = l->pending;
    l->pending = NULL;
    tz_obj_release((TzObject *)p);
  }
  if (l->drained) {
    TzPromise *p = l->drained;
    l->drained = NULL;
    tz_obj_release((TzObject *)p);
  }
  l->orphan = 1;
  if (!l->ops) {
    listener_free(l);
  }
}

typedef struct Sweep {
  TzObject base;
  NetListener *l;
} Sweep;

static void sweep_drop(void *object) { tz_obj_free(object); }

static const TzObjectDesc sweep_desc = {sweep_drop};

static void sweep_fired(TzObject *env) {
  NetListener *l = ((Sweep *)env)->l;
  double now = net_now();
  for (NetConn *c = l->conns; c; c = c->next) {
    conn_lock(c);
    if (c->h2) {
      if (!c->closed && tz_h2_idle(c) && l->keep_alive > 0 &&
          now - c->last_active >= l->keep_alive) {
        tz_h2_shutdown(c);
      }
      conn_unlock(c);
      continue;
    }
    if (!c->closed && c->busy && l->request_timeout > 0 && c->http.request_since > 0 &&
        !c->http.body_done && now - c->http.request_since >= l->request_timeout) {
      tz_http_request_too_slow(c);
    }
    if (!c->closed && !c->busy && !c->writing) {
      if (c->http.head_since > 0) {
        if (l->headers_timeout > 0 && now - c->http.head_since >= l->headers_timeout) {
          tz_http_too_slow(c);
        }
      } else if (l->keep_alive > 0 && now - c->last_active >= l->keep_alive) {
        net_abort(c);
      }
    }
    conn_unlock(c);
  }
}

static const TzFunctionInfo sweep_info = {(void *)sweep_fired, {NULL, 0}};

static void start_sweep(NetListener *l) {
  double every = 1000;
  if (l->keep_alive > 0 && l->keep_alive < every) {
    every = l->keep_alive;
  }
  if (l->headers_timeout > 0 && l->headers_timeout < every) {
    every = l->headers_timeout;
  }
  if (l->request_timeout > 0 && l->request_timeout < every) {
    every = l->request_timeout;
  }
  if (every < 10) {
    every = 10;
  }
  if (l->keep_alive <= 0 && l->headers_timeout <= 0 && l->request_timeout <= 0) {
    return;
  }
  Sweep *sweep = (Sweep *)tz_obj_alloc(sizeof(Sweep));
  sweep->base.rc = 1;
  sweep->base.desc = &sweep_desc;
  sweep->l = l;
  l->sweep = tz_timer_new(&sweep_info, &sweep->base, every, 1);
  tz_timer_ref(l->sweep, 0);
}

void *tz_net_listen(void *host_obj, uint64_t host_meta, int64_t port, double max_body,
                    int64_t keep_alive, int64_t headers_timeout, int64_t request_timeout,
                    TzMaker make) {
  TZ_ORDERED();
  start();
  tz_net_make = make;
  char host[256];
  if (!ascii_of(host_obj, host_meta, host, sizeof host)) {
    TzSystemErrorParts parts;
    memset(&parts, 0, sizeof parts);
    parts.message = ascii("the hostname to listen on must be ASCII");
    parts.errno_value = net_error_of(NET_NOT_FOUND)->number;
    parts.code = ascii("ERR_INVALID_ARG_VALUE");
    parts.syscall = ascii("listen");
    tz_failed = make(&parts);
    return NULL;
  }
  SOCKADDR_STORAGE address;
  int length = 0;
  if (!parse_literal(host, port, &address, &length)) {
    ADDRINFOW hints, *found = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    wchar_t wide[256];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, wide, 256);
    int rc = GetAddrInfoW(wide, NULL, &hints, &found);
    if (rc != 0 || !found) {
      tz_failed = net_system_error(make, NET_NOT_FOUND, "getaddrinfo", host, 0);
      return NULL;
    }
    ADDRINFOW *pick = found;
    for (ADDRINFOW *a = found; a; a = a->ai_next) {
      if (a->ai_family == AF_INET) {
        pick = a;
        break;
      }
    }
    memcpy(&address, pick->ai_addr, pick->ai_addrlen);
    length = (int)pick->ai_addrlen;
    ((SOCKADDR_IN *)&address)->sin_port = htons((u_short)port);
    FreeAddrInfoW(found);
  }
  char where[300];
  char text[64];
  address_text((SOCKADDR *)&address, text, sizeof text);
  snprintf(where, sizeof where, "%s:%lld", text, (long long)port);
  int family = address.ss_family;
  SOCKET s = WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  if (s == INVALID_SOCKET) {
    tz_failed = net_system_error(make, WSAGetLastError(), "listen", where, 1);
    return NULL;
  }
  if (family == AF_INET6) {
    DWORD no = 0;
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&no, sizeof no);
  }
  BOOL yes = TRUE;
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&yes, sizeof yes);
  if (bind(s, (SOCKADDR *)&address, length) == SOCKET_ERROR || listen(s, 511) == SOCKET_ERROR ||
      !attach(s)) {
    int error = WSAGetLastError();
    closesocket(s);
    tz_failed = net_system_error(make, error, "listen", where, 1);
    return NULL;
  }
  load_extensions(s);
  NetListener *l = (NetListener *)tz_obj_alloc(sizeof(NetListener));
  memset(l, 0, sizeof *l);
  l->rc = 1;
  l->layout = &listener_layout;
  l->s = s;
  l->accepting = INVALID_SOCKET;
  l->family = family;
  l->max_body = max_body;
  l->keep_alive = (double)keep_alive;
  l->headers_timeout = (double)headers_timeout;
  l->request_timeout = (double)request_timeout;
  memcpy(l->host, host, strlen(host) + 1);
  SOCKADDR_STORAGE bound;
  int bound_length = sizeof bound;
  getsockname(s, (SOCKADDR *)&bound, &bound_length);
  l->port = address_port((SOCKADDR *)&bound);
  start_sweep(l);

  return l;
}

void tz_net_listener_url(TzStr *out, void *listener, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  NetListener *l = (NetListener *)listener;
  const char *host = l->host;
  if (strcmp(host, "0.0.0.0") == 0 || strcmp(host, "::") == 0) {
    host = "localhost";
  }
  int bracket = strchr(host, ':') != NULL;
  char text[320];
  int n = snprintf(text, sizeof text, "%s://%s%s%s:%d/", l->tls ? "https" : "http",
                   bracket ? "[" : "", host, bracket ? "]" : "", (int)l->port);
  *out = tz_str_from_ascii(text, (size_t)n);
}

void tz_net_client_address(TzStr *out, void *conn, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;
  *out = tz_str_from_ascii(c->peer, strlen(c->peer));
}

int64_t tz_net_client_port(void *conn, TzMaker make) {
  (void)make;

  return ((NetConn *)conn)->peer_port;
}

void tz_net_client_protocol(TzStr *out, void *conn, TzMaker make) {
  (void)make;
  const char *protocol = ((NetConn *)conn)->h2parent ? "h2" : "http/1.1";
  *out = tz_str_from_ascii(protocol, strlen(protocol));
}

int64_t tz_net_listener_port(void *listener, TzMaker make) {
  TZ_ORDERED();
  (void)make;

  return ((NetListener *)listener)->port;
}

static void accept_done(TzWork *w);
static void accepted(NetListener *l, int32_t error);

static void settle_none(NetListener *l) {
  tz_lock(&l->ready_lock);
  if (l->pending) {
    void *none = NULL;
    net_fulfill(&l->pending, &none, sizeof none);
  }
  tz_unlock(&l->ready_lock);
}

static void accept_next(NetListener *l) {
  for (;;) {
    if (l->closed) {
      settle_none(l);
      return;
    }
    l->accepting = WSASocketW(l->family, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                              WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (l->accepting == INVALID_SOCKET) {
      settle_none(l);
      return;
    }
    DWORD got = 0;
    op_begin(&l->aop, accept_done, 0);
    ++l->ops;
    BOOL ok = tz_net_accept_ex(l->s, l->accepting, l->addresses, 0, sizeof(SOCKADDR_STORAGE) + 16,
                               sizeof(SOCKADDR_STORAGE) + 16, &got, &l->aop.ov);
    if (!ok && WSAGetLastError() == ERROR_IO_PENDING) {
      op_wait(&l->aop);
      return;
    }
    --l->ops;
    if (!ok) {
      closesocket(l->accepting);
      l->accepting = INVALID_SOCKET;
      continue;
    }
    accepted(l, 0);
    return;
  }
}

static void accept_done(TzWork *w) {
  NetListener *l = CONTAINING_RECORD(CONTAINING_RECORD(w, NetOp, work), NetListener, aop);
  --l->ops;
  accepted(l, op_error(&l->aop, l->s, l->closed));
}

static void accepted(NetListener *l, int32_t error) {
  SOCKET s = l->accepting;
  l->accepting = INVALID_SOCKET;
  if (l->orphan) {
    closesocket(s);
    if (!l->ops) {
      listener_free(l);
    }
    return;
  }
  if (error || l->closed) {
    closesocket(s);
    if (l->closed) {
      settle_none(l);
      return;
    }
    accept_next(l);
    return;
  }
  setsockopt(s, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (const char *)&l->s, sizeof l->s);
  void *pool = NULL;
#if TZ_PARALLEL
  if (!l->tls) {
    pool = tz_pool_port_handle();
  }
#endif
  int attached = 0;
#if TZ_PARALLEL
  if (pool) {
    attached = attach_to(s, (HANDLE)pool, net_completed_pooled);
  } else
#endif
    attached = attach(s);
  if (!attached) {
    closesocket(s);
    accept_next(l);
    return;
  }
  NetConn *c = conn_new(s);
  SOCKADDR *local = NULL, *remote = NULL;
  int local_length = 0, remote_length = 0;
  tz_net_accept_addresses(l->addresses, 0, sizeof(SOCKADDR_STORAGE) + 16,
                          sizeof(SOCKADDR_STORAGE) + 16, &local, &local_length, &remote,
                          &remote_length);
  if (remote) {
    address_text(remote, c->peer, sizeof c->peer);
    c->peer_port = address_port(remote);
  }
  c->pooled = pool != NULL;
  c->server = 1;
  c->listener = l;
  c->next = l->conns;
  if (l->conns) {
    l->conns->prev = c;
  }
  l->conns = c;
  c->http.max_body = l->max_body;
  c->http.request_timeout = l->request_timeout;
  if (l->tls) {
    l->tls_accept(l, c);
  }
  net_accept_ready(l, c);
}

void tz_net_accept(TzPromise *p, void *listener, TzMaker make) {
  TZ_ORDERED();
  NetListener *l = (NetListener *)listener;
  tz_net_make = make;
  tz_ref_retain(p);
  tz_lock(&l->ready_lock);
  l->pending = p;
  NetConn *ready = take_ready(l);
  if (ready) {
    net_fulfill(&l->pending, &ready, sizeof ready);
    tz_unlock(&l->ready_lock);
    return;
  }
  tz_unlock(&l->ready_lock);
  if (l->accepting == INVALID_SOCKET) {
    accept_next(l);
  }
}

void tz_net_listen_http2(void *listener, int64_t flags, int64_t streams, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  NetListener *l = (NetListener *)listener;
  l->h2 = (flags & 1) != 0;
  l->h2c = (flags & 2) != 0;
  l->h2only = (flags & 4) != 0;
  l->h2_streams = streams > 0 ? (uint32_t)streams : 100;
  if (l->tls && l->tls_alpn) {
    l->tls_alpn(l);
  }
}

void tz_net_drained(TzPromise *p, void *listener, TzMaker make) {
  TZ_ORDERED();
  NetListener *l = (NetListener *)listener;
  tz_net_make = make;
  tz_ref_retain(p);
  if (l->drained) {
    TzPromise *old = l->drained;
    l->drained = NULL;
    net_fulfill(&old, NULL, 0);
  }
  l->drained = p;
  if (l->closed && (!l->conns || l->aborted)) {
    net_fulfill(&l->drained, NULL, 0);
  }
}

void tz_net_close_listener(void *listener, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  NetListener *l = (NetListener *)listener;
  listener_close(l);
  for (NetConn *c = l->conns; c; c = c->next) {
    conn_lock(c);
    c->http.ending = 1;
    c->http.keep_alive = 0;
    if (c->h2) {
      tz_h2_shutdown(c);
    } else if (!c->closed && !c->busy && !c->writing) {
      net_abort(c);
    }
    conn_unlock(c);
  }
}

void tz_net_abort_all(void *listener, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  NetListener *l = (NetListener *)listener;
  listener_close(l);
  l->aborted = 1;
  for (NetConn *c = l->conns; c; c = c->next) {
    conn_lock(c);
    c->http.ending = 1;
    c->http.keep_alive = 0;
    if (!c->closed) {
      net_abort(c);
    }
    conn_unlock(c);
  }
  if (l->drained) {
    net_fulfill(&l->drained, NULL, 0);
  }
}

typedef struct Connect {
  NetOp op;
  TzPromise *promise;
  TzMaker make;
  char host[256];
  int port;
  SOCKADDR_STORAGE addresses[16];
  int lengths[16];
  int count, next;
  NetConn *conn;
  int32_t error;
  int failed_at;
  TzDeadline *deadline;
  TzDeadlineListener abort;
  ADDRINFOEXW *found;
  HANDLE cancel;
  int looking;
  int done;
} Connect;

static void connect_free(Connect *r) {
  if (r->deadline) {
    tz_deadline_unlisten(&r->abort);
    tz_instance_release((TzInstance *)r->deadline);
  }
  if (r->conn) {
    r->conn->orphan = 1;
    close_socket(r->conn);
    conn_settle(r->conn);
  }
  if (r->promise) {
    tz_obj_release((TzObject *)r->promise);
  }
  tz_free(r);
}

static void connect_fail(Connect *r, int32_t error) {
  char where[300];
  char text[64];
  const char *what = r->host;
  if (r->count > 0) {
    address_text((SOCKADDR *)&r->addresses[r->failed_at], text, sizeof text);
    snprintf(where, sizeof where, "%s:%d", text, r->port);
    what = where;
  }
  void *e = error == NET_NOT_FOUND || error == NET_TRY_AGAIN
                ? net_system_error(r->make, error, "getaddrinfo", r->host, 0)
                : net_system_error(r->make, error, "connect", what, 0);
  net_reject(&r->promise, e);
}

static void connect_done(TzWork *w);
static void connected(Connect *r, int32_t error);

static void sort_addresses(Connect *r) {
  SOCKADDR_STORAGE sorted[16];
  int lengths[16], n = 0;
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 0; i < r->count; ++i) {
      if ((r->addresses[i].ss_family == AF_INET) == (pass == 0)) {
        sorted[n] = r->addresses[i];
        lengths[n++] = r->lengths[i];
      }
    }
  }
  memcpy(r->addresses, sorted, sizeof(SOCKADDR_STORAGE) * (size_t)n);
  memcpy(r->lengths, lengths, sizeof(int) * (size_t)n);
}

static void attempt_failed(Connect *r, int32_t error) {
  if (r->error) {
    return;
  }
  r->error = error;
  r->failed_at = r->next - 1;
}

static int is_loopback(const SOCKADDR_STORAGE *a) {
  if (a->ss_family == AF_INET) {
    return ((const uint8_t *)&((const SOCKADDR_IN *)a)->sin_addr)[0] == 127;
  }

  return IN6_IS_ADDR_LOOPBACK(&((const SOCKADDR_IN6 *)a)->sin6_addr);
}

static void connect_next(Connect *r) {
  while (r->next < r->count) {
    SOCKADDR_STORAGE *address = &r->addresses[r->next];
    int length = r->lengths[r->next++];
    if (r->conn) {
      r->conn->orphan = 1;
      close_socket(r->conn);
      conn_settle(r->conn);
      r->conn = NULL;
    }
    SOCKET s = WSASocketW(address->ss_family, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                          WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) {
      attempt_failed(r, WSAGetLastError());
      continue;
    }
    if (is_loopback(address)) {
      TCP_INITIAL_RTO_PARAMETERS rto;
      memset(&rto, 0, sizeof rto);
      rto.Rtt = TCP_INITIAL_RTO_UNSPECIFIED_RTT;
      rto.MaxSynRetransmissions = TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS;
      DWORD bytes;
      WSAIoctl(s, SIO_TCP_INITIAL_RTO, &rto, sizeof rto, NULL, 0, &bytes, NULL, NULL);
    }
    SOCKADDR_STORAGE any;
    memset(&any, 0, sizeof any);
    any.ss_family = address->ss_family;
    if (bind(s, (SOCKADDR *)&any, length) == SOCKET_ERROR || !attach(s)) {
      attempt_failed(r, WSAGetLastError());
      closesocket(s);
      continue;
    }
    load_extensions(s);
    r->conn = conn_new(s);
    op_begin(&r->op, connect_done, 0);
    ++r->conn->ops;
    BOOL ok = tz_net_connect_ex(s, (SOCKADDR *)address, length, NULL, 0, NULL, &r->op.ov);
    if (!ok && WSAGetLastError() == ERROR_IO_PENDING) {
      op_wait(&r->op);
      return;
    }
    --r->conn->ops;
    if (!ok) {
      attempt_failed(r, WSAGetLastError());
      continue;
    }
    connected(r, 0);
    return;
  }
  connect_fail(r, r->error ? r->error : WSAECONNREFUSED);
  connect_free(r);
}

static void connect_done(TzWork *w) {
  Connect *r = (Connect *)CONTAINING_RECORD(w, NetOp, work);
  --r->conn->ops;
  connected(r, op_error(&r->op, r->conn->s, r->conn->closed));
}

static void connected(Connect *r, int32_t error) {
  NetConn *c = r->conn;
  if (r->done) {
    connect_free(r);
    return;
  }
  if (error) {
    attempt_failed(r, error);
    connect_next(r);
    return;
  }
  setsockopt(c->s, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
  r->conn = NULL;
  net_fulfill(&r->promise, &c, sizeof c);
  connect_free(r);
}

static void names_done(TzWork *w) {
  Connect *r = (Connect *)CONTAINING_RECORD(w, NetOp, work);
  r->looking = 0;
  if (r->done) {
    if (r->found) {
      FreeAddrInfoExW(r->found);
    }
    connect_free(r);
    return;
  }
  int32_t error = r->op.error;
  r->count = 0;
  for (ADDRINFOEXW *a = r->found; a && r->count < 16; a = a->ai_next) {
    if ((a->ai_family != AF_INET && a->ai_family != AF_INET6) ||
        a->ai_addrlen > sizeof(SOCKADDR_STORAGE)) {
      continue;
    }
    memcpy(&r->addresses[r->count], a->ai_addr, a->ai_addrlen);
    ((SOCKADDR_IN *)&r->addresses[r->count])->sin_port = htons((u_short)r->port);
    r->lengths[r->count++] = (int)a->ai_addrlen;
  }
  if (r->found) {
    FreeAddrInfoExW(r->found);
  }
  r->found = NULL;
  if (r->count == 0) {
    connect_fail(r, error == WSATRY_AGAIN ? NET_TRY_AGAIN : NET_NOT_FOUND);
    connect_free(r);
    return;
  }
  sort_addresses(r);
  connect_next(r);
}

static void names_posted(OVERLAPPED_ENTRY *entry) {
  NetOp *op = CONTAINING_RECORD(entry->lpOverlapped, NetOp, ov);
  tz_io_due(&op->work);
}

static void CALLBACK found_names(DWORD error, DWORD bytes, LPWSAOVERLAPPED overlapped) {
  (void)bytes;
  NetOp *op = CONTAINING_RECORD(overlapped, NetOp, ov);
  op->error = (int32_t)error;
  PostQueuedCompletionStatus((HANDLE)tz_io_port_handle(), 0, (ULONG_PTR)names_posted, overlapped);
}

static void abort_connect(TzDeadlineListener *l) {
  Connect *r = CONTAINING_RECORD(l, Connect, abort);
  net_reject(&r->promise, net_timed_out());
  r->done = 1;
  if (r->looking && r->cancel) {
    GetAddrInfoExCancel(&r->cancel);
  }
  if (r->conn) {
    close_socket(r->conn);
  }
}

void tz_net_connect(TzPromise *p, void *host_obj, uint64_t host_meta, int64_t port, void *deadline,
                    TzMaker make) {
  TZ_ORDERED();
  start();
  tz_net_make = make;
  Connect *r = (Connect *)tz_alloc(sizeof(Connect));
  memset(r, 0, sizeof *r);
  tz_ref_retain(p);
  r->promise = p;
  r->make = make;
  r->port = port;
  if (!ascii_of(host_obj, host_meta, r->host, sizeof r->host)) {
    net_reject(&r->promise, net_failure("ERR_INVALID_ARG_VALUE", "the host name must be ASCII"));
    connect_free(r);
    return;
  }
  if (deadline) {
    if (tz_deadline_is_passed(deadline)) {
      net_reject(&r->promise, net_timed_out());
      connect_free(r);
      return;
    }
    tz_ref_retain(deadline);
    r->deadline = (TzDeadline *)deadline;
    r->abort.run = abort_connect;
    tz_deadline_listen(r->deadline, &r->abort);
  }
  int length;
  if (parse_literal(r->host, port, &r->addresses[0], &length)) {
    r->lengths[0] = length;
    r->count = 1;
    connect_next(r);
    return;
  }
  if (_stricmp(r->host, "localhost") == 0) {
    parse_literal("127.0.0.1", port, &r->addresses[0], &r->lengths[0]);
    parse_literal("::1", port, &r->addresses[1], &r->lengths[1]);
    r->count = 2;
    connect_next(r);
    return;
  }
  wchar_t wide[256];
  MultiByteToWideChar(CP_UTF8, 0, r->host, -1, wide, 256);
  ADDRINFOEXW hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  op_begin(&r->op, names_done, 0);
  r->looking = 1;
  INT rc = GetAddrInfoExW(wide, NULL, NS_ALL, NULL, &hints, &r->found, NULL, &r->op.ov, found_names,
                          &r->cancel);
  if (rc == WSA_IO_PENDING) {
    op_wait(&r->op);
    return;
  }
  r->op.error = rc;
  names_done(&r->op.work);
}

#else

TZ_NORETURN static void unsupported(void) {
  tz_panicf("networking is not supported on this system yet (docs/net.md, step 1)");
}

void net_read(NetConn *c) { (void)c; }
void net_input_room(NetConn *c, uint32_t room) {
  (void)c;
  (void)room;
}
uint8_t *net_seal_reserve(NetConn *c, size_t length) {
  (void)c;
  (void)length;

  return NULL;
}
void net_write(NetConn *c, const void *data, size_t length) {
  (void)c;
  (void)data;
  (void)length;
}
void net_flush(NetConn *c) { (void)c; }
uint8_t *net_reserve(NetConn *c, size_t length) {
  (void)c;
  (void)length;

  return NULL;
}
void net_wrote(NetConn *c, size_t length) {
  (void)c;
  (void)length;
}
size_t net_pending_output(const NetConn *c) {
  (void)c;

  return 0;
}
void net_close(NetConn *c) { (void)c; }
void net_abort(NetConn *c) { (void)c; }
void tz_net_close(void *conn, TzMaker make) {
  TZ_ORDERED();
  (void)conn;
  (void)make;
}
void *tz_net_listen(void *host_obj, uint64_t host_meta, int64_t port, double max_body,
                    int64_t keep_alive, int64_t headers_timeout, int64_t request_timeout,
                    TzMaker make) {
  (void)host_obj;
  (void)host_meta;
  (void)port;
  (void)max_body;
  (void)keep_alive;
  (void)headers_timeout;
  (void)request_timeout;
  (void)make;
  unsupported();

  return NULL;
}
void tz_net_listener_url(TzStr *out, void *listener, TzMaker make) {
  (void)out;
  (void)listener;
  (void)make;
  unsupported();
}
void tz_net_client_address(TzStr *out, void *conn, TzMaker make) {
  (void)out;
  (void)conn;
  (void)make;
  unsupported();
}
int64_t tz_net_client_port(void *conn, TzMaker make) {
  (void)conn;
  (void)make;

  return 0;
}
void tz_net_client_protocol(TzStr *out, void *conn, TzMaker make) {
  (void)out;
  (void)conn;
  (void)make;
}
int64_t tz_net_listener_port(void *listener, TzMaker make) {
  TZ_ORDERED();
  (void)listener;
  (void)make;

  return 0;
}
void tz_net_accept(TzPromise *p, void *listener, TzMaker make) {
  TZ_ORDERED();
  (void)listener;
  (void)make;
  unsupported();
}
void tz_net_close_listener(void *listener, TzMaker make) {
  TZ_ORDERED();
  (void)listener;
  (void)make;
}
void tz_net_abort_all(void *listener, TzMaker make) {
  TZ_ORDERED();
  (void)listener;
  (void)make;
}
void tz_net_listen_http2(void *listener, int64_t flags, int64_t streams, TzMaker make) {
  (void)listener;
  (void)flags;
  (void)streams;
  (void)make;
}
NetConn *net_stream_new(NetConn *parent) {
  (void)parent;
  unsupported();

  return NULL;
}
void net_accept_ready(NetListener *l, NetConn *c) {
  (void)l;
  (void)c;
}
void tz_net_drained(TzPromise *p, void *listener, TzMaker make) {
  TZ_ORDERED();
  (void)p;
  (void)listener;
  (void)make;
  unsupported();
}
void tz_net_connect(TzPromise *p, void *host_obj, uint64_t host_meta, int64_t port, void *deadline,
                    TzMaker make) {
  TZ_ORDERED();
  (void)host_obj;
  (void)host_meta;
  (void)port;
  (void)deadline;
  (void)make;
  unsupported();
}

#endif
