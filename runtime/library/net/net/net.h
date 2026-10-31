#ifndef TZ_NET_H
#define TZ_NET_H

#include "runtime/core/array/array.h"
#include "runtime/core/async/async.h"
#include "runtime/core/io/io.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/pendulum/pendulum.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include "runtime/library/net/deadline/deadline.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#include <windows.h>
#endif

#ifndef CONTAINING_RECORD
#define CONTAINING_RECORD(address, type, field)                                                    \
  ((type *)((char *)(address) - offsetof(type, field)))
#endif

#define NET_HIGH_WATER_MARK 65536

typedef struct NetConn NetConn;
typedef struct NetListener NetListener;

#define NET_TLS_FAILED (-2)

typedef struct NetTlsOps {
  int (*open)(NetConn *c);
  void (*seal)(NetConn *c);
  void (*shutdown)(NetConn *c);
  void *(*error)(NetConn *c, TzMaker make);
  void (*release)(NetConn *c);
  int (*h2)(NetConn *c);
} NetTlsOps;

typedef struct NetOp {
  TzWork work;
#ifdef _WIN32
  OVERLAPPED ov;
#endif
  uint32_t bytes;
  int32_t error;
  int active;
} NetOp;

typedef struct HttpState {
  TzStr method, target, status_text;
  TzArray *headers;
  int32_t status;
  int32_t version;
  uint8_t keep_alive;
  uint8_t head_request;
  uint8_t expect_continue;
  uint8_t continue_sent;
  uint8_t body;
  uint8_t chunk_state;
  uint8_t body_done;
  uint8_t response_mode;
  uint8_t refused;
  uint8_t ending;
  int64_t remaining;
  int64_t body_read;
  double max_body;
  TzPromise *waiting;
  int32_t wait_kind;
  TzMaker make;
  TzDeadline *deadline;
  TzDeadlineListener abort;
  uint8_t *all;
  size_t all_length, all_capacity;
  double head_since;
  double request_since;
  double request_timeout;
  uint8_t responded;
} HttpState;

struct NetConn {
  int64_t rc;
  const TzLayout *layout;
#ifdef _WIN32
  SOCKET s;
#else
  int s;
#endif
  TzLock lock;
  uint8_t pooled;
  uint8_t closed;
  uint8_t closing;
  uint8_t orphan;
  uint8_t eof;
  uint8_t reading, writing;
  uint8_t server;
  int32_t error;
  int32_t ops;
  NetListener *listener;
  NetConn *prev, *next;
  double last_active;
  uint8_t busy;
  uint8_t *in;
  uint32_t in_start, in_end, in_cap;
  NetOp rop;
  void (*on_input)(NetConn *c);
  uint8_t *out, *flight;
  uint32_t out_len, out_cap, flight_len, flight_cap;
  NetOp wop;
  TzPromise *drain;
  HttpState http;
  void *tls;
  const NetTlsOps *tls_ops;
  uint8_t *raw;
  uint32_t raw_start, raw_end, raw_cap;
  uint8_t raw_eof;
  uint8_t raw_full;
  uint8_t *sealed;
  uint32_t sealed_len, sealed_cap;
  char peer[64];
  int32_t peer_port;
  void *h2;
  NetConn *h2parent;
  NetConn *ready_next;
  int32_t h2id;
  uint8_t h2fresh;
  uint8_t h2ended;
  uint8_t h2open;
  uint8_t h2submitted;
  uint8_t h2offer;
  TzWeight weight;
};

static inline TzLock *conn_lock_of(NetConn *c) {
  return c->h2parent ? &c->h2parent->lock : &c->lock;
}

static inline void conn_lock(NetConn *c) {
#if TZ_PARALLEL && !defined(TZ_NO_THREAD_STATE)
  tz_lock(conn_lock_of(c));
  ++tz_thread.conn_locks;
#else
  (void)c;
#endif
}

static inline void conn_unlock(NetConn *c) {
#if TZ_PARALLEL && !defined(TZ_NO_THREAD_STATE)
  --tz_thread.conn_locks;
  tz_unlock(conn_lock_of(c));
#else
  (void)c;
#endif
}

int tz_http_waits_request(NetConn *c);

struct NetListener {
  int64_t rc;
  const TzLayout *layout;
#ifdef _WIN32
  SOCKET s;
  SOCKET accepting;
  uint8_t addresses[2 * (sizeof(SOCKADDR_STORAGE) + 16)];
#else
  int s;
#endif
  int32_t family;
  int32_t port;
  uint8_t closed, orphan, aborted;
  int32_t ops;
  NetOp aop;
  TzPromise *pending;
  NetConn *conns;
  TzPromise *drained;
  double max_body;
  double keep_alive;
  double headers_timeout;
  double request_timeout;
  struct TzTimer *sweep;
  char host[256];
  void *tls;
  void (*tls_accept)(NetListener *l, NetConn *c);
  void (*tls_release)(void *tls);
  void (*tls_alpn)(NetListener *l);
  NetConn *ready_first, *ready_last;
  TzLock ready_lock;
  uint8_t h2, h2c, h2only;
  uint32_t h2_streams;
};

double net_now(void);

void net_read(NetConn *c);
void net_input_room(NetConn *c, uint32_t room);
uint8_t *net_seal_reserve(NetConn *c, size_t length);
void net_write(NetConn *c, const void *data, size_t length);
void net_flush(NetConn *c);
void net_hold(NetConn *c);
uint8_t *net_reserve(NetConn *c, size_t length);
void net_wrote(NetConn *c, size_t length);
size_t net_pending_output(const NetConn *c);
void net_close(NetConn *c);
void net_abort(NetConn *c);
NetConn *net_stream_new(NetConn *parent);
void net_accept_ready(NetListener *l, NetConn *c);

typedef struct NetError {
  const char *code, *message;
  int32_t number;
} NetError;
const NetError *net_error_of(int32_t error);
void *net_system_error(TzMaker make, int32_t error, const char *syscall, const char *where,
                       int with_text);
void *net_failure(const char *code, const char *message);
void *net_timed_out(void);
TzStr net_utf8(const char *text);

void net_fulfill(TzPromise **p, const void *value, size_t size);
void net_reject(TzPromise **p, void *error);

TzBytes *net_bytes(const uint8_t *data, size_t length);

extern TzMaker tz_net_make;

void *tz_net_listen(void *host_obj, uint64_t host_meta, int64_t port, double max_body,
                    int64_t keep_alive, int64_t headers_timeout, int64_t request_timeout,
                    TzMaker make);
void tz_http_too_slow(NetConn *c);
void tz_http_request_too_slow(NetConn *c);

#ifdef _WIN32
void net_start(void);
int net_attach(SOCKET s);
void net_op_begin(NetOp *op, void (*done)(TzWork *), int unref);
void net_op_wait(NetOp *op);
int32_t net_op_error(NetOp *op, SOCKET s, int closed);
void net_address_text(const SOCKADDR *sa, char *out, size_t size);
int net_address_port(const SOCKADDR *sa);
int net_ascii_of(void *obj, uint64_t meta, char *out, size_t size);
int net_parse_literal(const char *host, int port, SOCKADDR_STORAGE *out, int *length);
#endif

#endif
