// TCP sockets of the runtime (net.c) and what HTTP (http.c) builds on them;
// shared by those two files only. See net.c.

#ifndef TZ_NET_H
#define TZ_NET_H

#include "runtime/core/tzrt/tzrt.h"

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

// What may be queued to write before a body that streams waits.
#define NET_HIGH_WATER_MARK 65536

typedef struct NetConn NetConn;
typedef struct NetListener NetListener;

// The error of a connection whose TLS failed (NetConn::error): the TLS layer
// tells what it was (NetTlsOps::error).
#define NET_TLS_FAILED (-2)

// What net.c asks of the TLS of a connection: tls.c, which only programs
// that use https have (docs/net.md, section 4), so net.c reaches it through
// these. TLS sits between the bytes of the socket and those of the program:
// the socket reads ciphertext into `raw`, which `open` decrypts into the
// input; `seal` encrypts the output into `sealed`, which the socket writes.
typedef struct NetTlsOps {
  // Decrypts what was read; 1 if that gives the reader something to look
  // at: plaintext in the input, its end (c->eof), an error (c->error), or a
  // handshake just done. 0 if more must be read first.
  int (*open)(NetConn *c);
  // Encrypts what the program queued to write (once the handshake is done).
  void (*seal)(NetConn *c);
  // Ends the TLS of the connection (close_notify), before the socket closes.
  void (*shutdown)(NetConn *c);
  // The error of NET_TLS_FAILED, as node words it.
  void *(*error)(NetConn *c, TzMaker make);
  // Frees what the TLS of the connection holds.
  void (*release)(NetConn *c);
} NetTlsOps;

// An operation that waits for the system: the work the loop hands back
// (first, see net.c), and what the system said.
typedef struct NetOp {
  TzWork work;
#ifdef _WIN32
  OVERLAPPED ov;
#endif
  uint32_t bytes;
  int32_t error; // of the system, 0 if none
  int active;
} NetOp;

// What HTTP keeps of a connection (http.c).
typedef struct HttpState {
  // The head of the message last read: owned strings, and the headers as a
  // string[] of names (lowercase) and values, one after the other.
  TzStr method, target, status_text;
  TzArray *headers;
  int32_t status;
  int32_t version;      // 10 or 11
  uint8_t keep_alive;   // the connection may carry another message
  uint8_t head_request; // the request was HEAD: its response has no body
  uint8_t expect_continue;
  uint8_t continue_sent;
  // The body: none, of `remaining` bytes, chunked, or up to the end of the
  // connection (a response without a length).
  uint8_t body;
  uint8_t chunk_state;
  uint8_t body_done;
  uint8_t response_mode; // of the response being written (http.c)
  uint8_t refused;       // answered with an error already: what the program writes goes
  uint8_t ending;        // the server stops: no request after the one being handled
  int64_t remaining;
  int64_t body_read; // bytes of the body given so far
  double max_body;   // of a request to a server; 0: no limit
  // A promise of the program waiting for the next head, body chunk or
  // whole body, and what it waits for.
  TzPromise *waiting;
  int32_t wait_kind;
  TzMaker make; // of SystemError, for the promise
  TzSignal *signal;
  TzAbortListener abort;
  // Bytes of the whole body read so far (readAll).
  uint8_t *all;
  size_t all_length, all_capacity;
} HttpState;

struct NetConn {
  int64_t rc;
  const TzLayout *layout;
#ifdef _WIN32
  SOCKET s;
#else
  int s;
#endif
  // The parallel runtime (docs/async.md, section 8): what the connection
  // holds changes under the lock, as the thread that runs its task and
  // those that take what the system says of it may differ. A connection of
  // a server waits on the port of the pool (`pooled`): its threads take
  // what comes for it, and run the piece that waits for it (net.c).
  TzLock lock;
  uint8_t pooled;
  uint8_t closed;  // the socket is closed
  uint8_t closing; // closes once what is queued is written
  uint8_t orphan;  // the program let go of it: freed once no operation waits
  uint8_t eof;     // the other side ended its half
  uint8_t reading, writing;
  uint8_t server;        // accepted by a listener
  int32_t error;         // the error of the system that broke it, 0 if none
  int32_t ops;           // operations in flight
  NetListener *listener; // of a server's connection, while both live
  NetConn *prev, *next;  // in the listener's list
  double last_active;    // when it last read or wrote, in ms of the clock
  uint8_t busy;          // a request is being handled
  // Input: the bytes from `in_start` to `in_end` are read and not used yet.
  uint8_t *in;
  uint32_t in_start, in_end, in_cap;
  NetOp rop;
  // What waits for input: called when more came, at its end, or on an error.
  void (*on_input)(NetConn *c);
  // Output: `out` is queued, `flight` handed to the system.
  uint8_t *out, *flight;
  uint32_t out_len, out_cap, flight_len, flight_cap;
  NetOp wop;
  TzPromise *drain; // waits until the output is below the high-water mark
  HttpState http;
  // TLS, or NULL: what tls.c keeps of it, and the ciphertext on either side,
  // from `raw_start` to `raw_end` read and not decrypted yet (the socket
  // ended after it when `raw_eof`), and `sealed` queued to write, which goes
  // out through `flight` as the output of a plain connection does.
  void *tls;
  const NetTlsOps *tls_ops;
  uint8_t *raw;
  uint32_t raw_start, raw_end, raw_cap;
  uint8_t raw_eof;
  uint8_t raw_full; // the last read filled `raw`: the next gets more room
  uint8_t *sealed;
  uint32_t sealed_len, sealed_cap;
};

// The lock of a connection (see NetConn). A thread that holds it never
// waits for its turn (parallel.c checks), so that the main thread may wait
// for it.
static inline void conn_lock(NetConn *c) {
#if TZ_PARALLEL && !defined(TZ_NO_THREAD_STATE)
  tz_lock(&c->lock);
  ++tz_thread.conn_locks;
#else
  (void)c;
#endif
}

static inline void conn_unlock(NetConn *c) {
#if TZ_PARALLEL && !defined(TZ_NO_THREAD_STATE)
  --tz_thread.conn_locks;
  tz_unlock(&c->lock);
#else
  (void)c;
#endif
}

// http.c: whether the connection of a server waits for the head of its
// next request.
int tz_http_waits_request(NetConn *c);

struct NetListener {
  int64_t rc;
  const TzLayout *layout;
#ifdef _WIN32
  SOCKET s;
  SOCKET accepting; // the socket AcceptEx fills
  uint8_t addresses[2 * (sizeof(SOCKADDR_STORAGE) + 16)];
#else
  int s;
#endif
  int32_t family;
  int32_t port;
  uint8_t closed, orphan;
  int32_t ops;
  NetOp aop;
  TzPromise *pending; // accept() of the program, waiting
  NetConn *conns;     // the connections it accepted that are open
  double max_body;
  // The TLS of a server of https (tls.c): what each connection accepted
  // starts with, and how it goes with the listener.
  void *tls;
  void (*tls_accept)(NetListener *l, NetConn *c);
  void (*tls_release)(void *tls);
};

// The clock, in ms, for idle connections.
double net_now(void);

// Reads more into the input of `c`; then c->on_input runs, also at the end
// of the input (c->eof) or on an error (c->error).
void net_read(NetConn *c);
// Room for at least `room` more bytes at the end of the input.
void net_input_room(NetConn *c, uint32_t room);
// Room for `length` more bytes of ciphertext at the end of `sealed`.
uint8_t *net_seal_reserve(NetConn *c, size_t length);
// Queues bytes to write, and writes what is queued.
void net_write(NetConn *c, const void *data, size_t length);
void net_flush(NetConn *c);
// Room for `length` more bytes at the end of the output, which net_wrote
// then counts as queued.
uint8_t *net_reserve(NetConn *c, size_t length);
void net_wrote(NetConn *c, size_t length);
// The output queued and in flight.
size_t net_pending_output(const NetConn *c);
// Closes the connection once what is queued is written.
void net_close(NetConn *c);
// Closes it now.
void net_abort(NetConn *c);

// Errors: the code, text and number of libuv for an error of the system, as
// node shows them; and a SystemError made with them.
typedef struct NetError {
  const char *code, *message;
  int32_t number;
} NetError;
const NetError *net_error_of(int32_t error);
// "connect ECONNREFUSED 127.0.0.1:80" (`with_text` adds the text after the
// code, as in "listen EADDRINUSE: address already in use :::80"); `where`
// may be NULL.
void *net_system_error(TzMaker make, int32_t error, const char *syscall, const char *where,
                       int with_text);
// An Error of `kind` with `message` (UTF-8) and, if not NULL, a cause, which
// it takes over.
void *net_error(int32_t kind, const char *message, void *cause);
// A string of UTF-8 text.
TzStr net_utf8(const char *text);

// Promises the runtime holds.
void net_fulfill(TzPromise **p, const void *value, size_t size);
void net_reject(TzPromise **p, void *error);

// The reason of an aborted signal, retained (signal.c).
void *tz_signal_reason(const void *signal);

// Bytes as a Uint8Array.
TzBytes *net_bytes(const uint8_t *data, size_t length);

// The SystemError maker of the last program call, for errors outside one.
extern TzMaker tz_net_make;

// `__tz_net_listen(host, port, maxBody)`, on which a server of https starts
// (tls.c).
void *tz_net_listen(void *host_obj, uint64_t host_meta, int32_t port, double max_body,
                    TzMaker make);

#endif
