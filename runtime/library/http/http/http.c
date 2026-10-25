// HTTP/1.1 on the connections of net.c (docs/net.md, steps 2 and 3): the
// heads of requests and responses read and checked, bodies of a length or
// in chunks, and heads written in one piece with the body. The program's
// side (Request, Response, serve, fetch) is in library/, which calls the
// `__tz_http_*` functions here.
//
// Reading is strict, as a server facing the internet must be (RFC 9112 and
// the rules against request smuggling): lines end with CRLF, a name of a
// header is a token right before its colon, no folded lines, no control
// characters in values, one Content-Length (or the same one repeated) and
// never together with Transfer-Encoding, which must end with chunked, a Host
// in every HTTP/1.1 request, and limits: 16 KB for the head (node's
// maxHeaderSize) and maxBodySize for a body. A request that breaks them is
// answered with 400 (431 for a head too big, 413 for a body, 501 for a
// transfer coding other than chunked, 505 for a version other than 1.x) and
// its connection closed.

#include "runtime/library/net/net/net.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/async/async.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include "runtime/library/events/signal/signal.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define HTTP_MAX_HEAD 16384
#define HTTP_MAX_HEADERS 1000
#define HTTP_MAX_LINE 4096       // a line of a chunk's size or of its trailer
#define HTTP_MAX_DISCARD 1048576 // of a body the program did not read, to keep the connection

// What a promise of the program waits for.
enum { WAIT_NONE, WAIT_REQUEST, WAIT_RESPONSE, WAIT_CHUNK, WAIT_ALL, WAIT_DRAIN };

// How a body is framed.
enum { BODY_NONE, BODY_LENGTH, BODY_CHUNKED, BODY_TO_END };

// Where the reading of chunks is.
enum { CHUNK_SIZE, CHUNK_DATA, CHUNK_DATA_END, CHUNK_TRAILER };

// How the response (or request) being written sends its body.
enum { SEND_FIXED, SEND_CHUNKED, SEND_RAW };

// The date of the Date header, made once a second.
TZ_THREAD_LOCAL char tz_http_date[32]; // each thread its own
TZ_THREAD_LOCAL int64_t tz_http_date_second = -1;

static TzStr ascii(const char *text) { return tz_str_from_ascii(text, strlen(text)); }

//===----------------------------------------------------------------------===//
// Characters
//===----------------------------------------------------------------------===//

// tchar of RFC 9110.
static int is_token(uint8_t c) {
  if (c >= 'a' && c <= 'z') {
    return 1;
  }
  if (c >= 'A' && c <= 'Z') {
    return 1;
  }
  if (c >= '0' && c <= '9') {
    return 1;
  }

  return c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' ||
         c == '+' || c == '-' || c == '.' || c == '^' || c == '_' || c == '`' || c == '|' ||
         c == '~';
}

// A character of a value: visible, a space, a tab, or obs-text.
static int is_value(uint8_t c) { return c == '\t' || (c >= 0x20 && c != 0x7F); }

static uint8_t lower(uint8_t c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; }

static int equals_lower(const uint8_t *s, size_t n, const char *lowered) {
  size_t m = strlen(lowered);
  if (n != m) {
    return 0;
  }
  for (size_t i = 0; i < n; ++i) {
    if (lower(s[i]) != (uint8_t)lowered[i]) {
      return 0;
    }
  }

  return 1;
}

static int is_space(uint8_t c) { return c == ' ' || c == '\t'; }

// Bytes as a string, each a character (latin1), as node reads heads.
static TzStr latin1(const uint8_t *s, size_t n) {
  int ascii_only = 1;
  for (size_t i = 0; i < n && ascii_only; ++i) {
    ascii_only = s[i] < 0x80;
  }
  if (ascii_only) {
    return tz_str_from_ascii((const char *)s, n);
  }
  TzBuf b;
  tz_buf_init(&b);
  tz_buf_bytes(&b, s, n);

  return tz_buf_take(&b);
}

static TzStr lowered(const uint8_t *s, size_t n) {
  char small[64];
  if (n <= sizeof small) {
    for (size_t i = 0; i < n; ++i) {
      small[i] = (char)lower(s[i]);
    }
    return tz_str_from_ascii(small, n);
  }
  char *big = (char *)tz_alloc(n);
  for (size_t i = 0; i < n; ++i) {
    big[i] = (char)lower(s[i]);
  }
  TzStr text = tz_str_from_ascii(big, n);
  tz_free(big);

  return text;
}

//===----------------------------------------------------------------------===//
// The state of a connection
//===----------------------------------------------------------------------===//

static void clear_head(HttpState *h) {
  tz_str_release_value(h->method);
  tz_str_release_value(h->target);
  tz_str_release_value(h->status_text);
  memset(&h->method, 0, sizeof h->method);
  memset(&h->target, 0, sizeof h->target);
  memset(&h->status_text, 0, sizeof h->status_text);
  if (h->headers && tz_rc_dec(&h->headers->rc)) {
    tz_arr_free(h->headers);
  }
  h->headers = NULL;
}

static void stop_listening(HttpState *h) {
  if (h->signal) {
    tz_signal_unlisten(&h->abort);
    tz_instance_release((TzInstance *)h->signal);
    h->signal = NULL;
  }
}

// What HTTP holds of a connection that goes (net.c).
void tz_http_release(NetConn *c) {
  HttpState *h = &c->http;
  clear_head(h);
  stop_listening(h);
  if (h->waiting) {
    TzPromise *p = h->waiting;
    h->waiting = NULL;
    tz_obj_release((TzObject *)p);
  }
  tz_free(h->all);
  h->all = NULL;
}

static void settle_value(NetConn *c, const void *value, size_t size) {
  HttpState *h = &c->http;
  stop_listening(h);
  h->wait_kind = WAIT_NONE;
  if (h->waiting) {
    net_fulfill(&h->waiting, value, size);
  }
}

static void settle_int(NetConn *c, int32_t value) { settle_value(c, &value, sizeof value); }

static void settle_error(NetConn *c, void *error) {
  HttpState *h = &c->http;
  stop_listening(h);
  h->wait_kind = WAIT_NONE;
  if (h->waiting) {
    net_reject(&h->waiting, error);
  } else {
    tz_instance_release((TzInstance *)error);
  }
}

// The connection broke while a client waited: "fetch failed" has this as
// its cause, as undici's SocketError "other side closed".
static void *broken_error(NetConn *c) {
  if (c->error == NET_TLS_FAILED) {
    return c->tls_ops->error(c, c->http.make);
  }
  if (c->error && c->error != -1) {
    return net_system_error(c->http.make, c->error, "read", NULL, 0);
  }

  return net_error(TZ_ERROR, "other side closed", NULL);
}

static void input(NetConn *c);

static void abort_wait(TzAbortListener *l) {
  HttpState *h = CONTAINING_RECORD(l, HttpState, abort);
  NetConn *c = CONTAINING_RECORD(h, NetConn, http);
  conn_lock(c);
  TzSignal *signal = h->signal;
  h->signal = NULL;
  void *reason = tz_signal_reason(signal);
  tz_instance_release((TzInstance *)signal);
  h->wait_kind = WAIT_NONE;
  if (h->waiting) {
    net_reject(&h->waiting, reason);
  } else {
    tz_instance_release((TzInstance *)reason);
  }
  // The rest of the message will not be read.
  h->keep_alive = 0;
  net_abort(c);
  conn_unlock(c);
}

// Starts a wait of the program: `p` (held), for `kind`, cancelled by
// `signal`.
static int begin_wait(NetConn *c, TzPromise *p, int kind, void *signal, TzMaker make) {
  HttpState *h = &c->http;
  tz_ref_retain(p); // it may be a member of a group (async.c)
  h->waiting = p;
  h->wait_kind = kind;
  h->make = make;
  tz_net_make = make;
  if (signal) {
    if (tz_signal_aborted(signal)) {
      h->wait_kind = WAIT_NONE;
      net_reject(&h->waiting, tz_signal_reason(signal));
      h->keep_alive = 0;
      net_abort(c);
      return 0;
    }
    tz_ref_retain(signal); // a member of a group (signal.c)
    h->signal = (TzSignal *)signal;
    h->abort.run = abort_wait;
    tz_signal_listen(h->signal, &h->abort);
  }
  c->on_input = input;

  return 1;
}

//===----------------------------------------------------------------------===//
// Heads
//===----------------------------------------------------------------------===//

// The end of a line at or after `at` before `end`: the position of its CR,
// which an LF follows. -1 if the line is not complete; -2 for a CR or an LF
// alone.
static ptrdiff_t line_end(const uint8_t *s, size_t at, size_t end) {
  for (size_t i = at; i < end; ++i) {
    if (s[i] == '\n') {
      return -2;
    }
    if (s[i] == '\r') {
      if (i + 1 == end) {
        return -1;
      }
      return s[i + 1] == '\n' ? (ptrdiff_t)i : -2;
    }
  }

  return -1;
}

typedef struct {
  const uint8_t *name, *value;
  uint32_t name_length, value_length;
} Field;

// What the headers say about the message.
typedef struct {
  int64_t length; // -1 without Content-Length
  int chunked;    // Transfer-Encoding ends with chunked
  int coded;      // it has another coding
  int close, keep_alive, hosts, expect_continue;
} Framing;

// A number of Content-Length: digits, maybe repeated with commas ("5, 5").
static int content_length(const uint8_t *v, size_t n, int64_t *out) {
  int64_t found = -1;
  size_t i = 0;
  while (i < n) {
    while (i < n && is_space(v[i])) {
      ++i;
    }
    size_t start = i;
    int64_t value = 0;
    while (i < n && v[i] >= '0' && v[i] <= '9') {
      if (value > (INT64_MAX - 9) / 10) {
        return 0;
      }
      value = value * 10 + (v[i] - '0');
      ++i;
    }
    if (i == start) {
      return 0;
    }
    while (i < n && is_space(v[i])) {
      ++i;
    }
    if (found >= 0 && found != value) {
      return 0;
    }
    found = value;
    if (i < n) {
      if (v[i] != ',') {
        return 0;
      }
      ++i;
    }
  }
  if (found < 0) {
    return 0;
  }
  *out = found;

  return 1;
}

// The codings of Transfer-Encoding, the last of which must be chunked.
static int transfer_encoding(const uint8_t *v, size_t n, Framing *f) {
  size_t i = 0;
  while (i < n) {
    while (i < n && (is_space(v[i]) || v[i] == ',')) {
      ++i;
    }
    size_t start = i;
    while (i < n && v[i] != ',' && v[i] != ';' && !is_space(v[i])) {
      ++i;
    }
    size_t length = i - start;
    while (i < n && v[i] != ',') { // parameters of a coding
      ++i;
    }
    if (!length) {
      continue;
    }
    if (f->chunked) { // chunked must be the last, and only once
      return 0;
    }
    if (equals_lower(v + start, length, "chunked")) {
      f->chunked = 1;
    } else {
      f->coded = 1;
    }
  }

  return 1;
}

static void connection_tokens(const uint8_t *v, size_t n, Framing *f) {
  size_t i = 0;
  while (i < n) {
    while (i < n && (is_space(v[i]) || v[i] == ',')) {
      ++i;
    }
    size_t start = i;
    while (i < n && v[i] != ',' && !is_space(v[i])) {
      ++i;
    }
    if (equals_lower(v + start, i - start, "close")) {
      f->close = 1;
    } else if (equals_lower(v + start, i - start, "keep-alive")) {
      f->keep_alive = 1;
    }
    while (i < n && v[i] != ',') {
      ++i;
    }
  }
}

// The header lines from `at` up to the empty line that ends the head at
// `end` (its CR): the fields, and what they say. 0 if one is malformed.
static int parse_fields(const uint8_t *s, size_t at, size_t end, TzArray **out, Framing *f,
                        int server) {
  memset(f, 0, sizeof *f);
  f->length = -1;
  TzArray *headers = tz_arr_new(&tz_string_type, 0, 16);
  uint32_t count = 0;
  int ok = 1;
  int lengths = 0, encodings = 0;
  while (at < end) {
    ptrdiff_t eol = line_end(s, at, end + 2);
    if (eol < 0) {
      ok = 0;
      break;
    }
    size_t line = (size_t)eol;
    // A folded line, or a space before the name.
    if (is_space(s[at])) {
      ok = 0;
      break;
    }
    size_t colon = at;
    while (colon < line && is_token(s[colon])) {
      ++colon;
    }
    if (colon == at || colon == line || s[colon] != ':') {
      ok = 0;
      break;
    }
    size_t v = colon + 1, ve = line;
    while (v < ve && is_space(s[v])) {
      ++v;
    }
    while (ve > v && is_space(s[ve - 1])) {
      --ve;
    }
    for (size_t k = v; k < ve; ++k) {
      if (!is_value(s[k])) {
        ok = 0;
        break;
      }
    }
    if (!ok) {
      break;
    }
    if (++count > HTTP_MAX_HEADERS) {
      ok = 0;
      break;
    }
    const uint8_t *name = s + at;
    size_t name_length = colon - at;
    if (equals_lower(name, name_length, "content-length")) {
      int64_t length;
      if (!content_length(s + v, ve - v, &length) || (f->length >= 0 && f->length != length)) {
        ok = 0;
        break;
      }
      f->length = length;
      ++lengths;
    } else if (equals_lower(name, name_length, "transfer-encoding")) {
      if (!transfer_encoding(s + v, ve - v, f)) {
        ok = 0;
        break;
      }
      ++encodings;
    } else if (equals_lower(name, name_length, "connection")) {
      connection_tokens(s + v, ve - v, f);
    } else if (equals_lower(name, name_length, "host")) {
      ++f->hosts;
    } else if (server && equals_lower(name, name_length, "expect")) {
      if (equals_lower(s + v, ve - v, "100-continue")) {
        f->expect_continue = 1;
      }
    }
    TzStr pair[2] = {lowered(name, name_length), latin1(s + v, ve - v)};
    tz_arr_push_value(headers, &pair[0]);
    tz_arr_push_value(headers, &pair[1]);
    at = line + 2;
  }
  (void)lengths;
  if (ok && encodings && !f->chunked && !f->coded) {
    f->coded = 1; // an empty Transfer-Encoding
  }
  if (!ok) {
    if (tz_rc_dec(&headers->rc)) {
      tz_arr_free(headers);
    }
    return 0;
  }
  *out = headers;

  return 1;
}

// Finds the empty line that ends the head in the input: the position of its
// CR, or -1 if it has not come yet.
static ptrdiff_t head_end(NetConn *c) {
  const uint8_t *s = c->in;
  for (size_t i = c->in_start; i + 3 < c->in_end; ++i) {
    if (s[i] == '\r' && s[i + 1] == '\n' && s[i + 2] == '\r' && s[i + 3] == '\n') {
      return (ptrdiff_t)(i + 2);
    }
  }

  return -1;
}

// "HTTP/1.1": the version, 10 or 11; 0 if malformed, -1 for another major.
static int version_of(const uint8_t *s, size_t n) {
  if (n != 8 || memcmp(s, "HTTP/", 5) != 0 || s[6] != '.' || s[5] < '0' || s[5] > '9' ||
      s[7] < '0' || s[7] > '9') {
    return 0;
  }
  if (s[5] != '1') {
    return -1;
  }

  return s[7] == '0' ? 10 : 11;
}

static const char *reason_of(int status) {
  switch (status) {
  case 100:
    return "Continue";
  case 101:
    return "Switching Protocols";
  case 102:
    return "Processing";
  case 103:
    return "Early Hints";
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 202:
    return "Accepted";
  case 203:
    return "Non-Authoritative Information";
  case 204:
    return "No Content";
  case 205:
    return "Reset Content";
  case 206:
    return "Partial Content";
  case 207:
    return "Multi-Status";
  case 208:
    return "Already Reported";
  case 226:
    return "IM Used";
  case 300:
    return "Multiple Choices";
  case 301:
    return "Moved Permanently";
  case 302:
    return "Found";
  case 303:
    return "See Other";
  case 304:
    return "Not Modified";
  case 305:
    return "Use Proxy";
  case 307:
    return "Temporary Redirect";
  case 308:
    return "Permanent Redirect";
  case 400:
    return "Bad Request";
  case 401:
    return "Unauthorized";
  case 402:
    return "Payment Required";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 406:
    return "Not Acceptable";
  case 407:
    return "Proxy Authentication Required";
  case 408:
    return "Request Timeout";
  case 409:
    return "Conflict";
  case 410:
    return "Gone";
  case 411:
    return "Length Required";
  case 412:
    return "Precondition Failed";
  case 413:
    return "Payload Too Large";
  case 414:
    return "URI Too Long";
  case 415:
    return "Unsupported Media Type";
  case 416:
    return "Range Not Satisfiable";
  case 417:
    return "Expectation Failed";
  case 418:
    return "I'm a Teapot";
  case 421:
    return "Misdirected Request";
  case 422:
    return "Unprocessable Entity";
  case 423:
    return "Locked";
  case 424:
    return "Failed Dependency";
  case 425:
    return "Too Early";
  case 426:
    return "Upgrade Required";
  case 428:
    return "Precondition Required";
  case 429:
    return "Too Many Requests";
  case 431:
    return "Request Header Fields Too Large";
  case 451:
    return "Unavailable For Legal Reasons";
  case 500:
    return "Internal Server Error";
  case 501:
    return "Not Implemented";
  case 502:
    return "Bad Gateway";
  case 503:
    return "Service Unavailable";
  case 504:
    return "Gateway Timeout";
  case 505:
    return "HTTP Version Not Supported";
  case 506:
    return "Variant Also Negotiates";
  case 507:
    return "Insufficient Storage";
  case 508:
    return "Loop Detected";
  case 509:
    return "Bandwidth Limit Exceeded";
  case 510:
    return "Not Extended";
  case 511:
    return "Network Authentication Required";
  default:
    return "";
  }
}

// `__tz_http_reason(status)`: the text node gives a status.
void tz_http_reason(TzStr *out, int32_t status, TzMaker make) {
  (void)make;
  *out = ascii(reason_of(status));
}

// A request the server cannot take: its answer, and the connection ends.
static void answer_error(NetConn *c, int status) {
  char text[160];
  int n = snprintf(text, sizeof text,
                   "HTTP/1.1 %d %s\r\nconnection: close\r\ncontent-length: 0\r\n\r\n", status,
                   reason_of(status));
  c->http.keep_alive = 0;
  c->http.refused = 1;
  net_write(c, text, (size_t)n);
  net_close(c);
}

static void refuse(NetConn *c, int status) {
  answer_error(c, status);
  settle_int(c, 0);
}

// Reads the head of a request; 1 once it is read, 0 if more input is
// needed, -1 if the request was refused or the connection ended.
static int read_request(NetConn *c) {
  HttpState *h = &c->http;
  // Empty lines before a request are allowed (RFC 9112, 2.2).
  while (c->in_end - c->in_start >= 2 && c->in[c->in_start] == '\r' &&
         c->in[c->in_start + 1] == '\n') {
    c->in_start += 2;
  }
  ptrdiff_t end = head_end(c);
  if (end < 0) {
    if (c->in_end - c->in_start > HTTP_MAX_HEAD) {
      refuse(c, 431);
      return -1;
    }
    // A request starts with its method, a token: anything else, such as the
    // handshake of TLS sent to a server of http, is answered at once, as node
    // does, rather than waited on for the end of a head that never comes.
    if (c->in_end > c->in_start && c->in[c->in_start] != '\r' && !is_token(c->in[c->in_start])) {
      refuse(c, 400);
      return -1;
    }
    // A line that ends with LF alone, or a CR alone, is no line of HTTP.
    for (size_t i = c->in_start; i < c->in_end; ++i) {
      if ((c->in[i] == '\n' && (i == c->in_start || c->in[i - 1] != '\r')) ||
          (c->in[i] == '\r' && i + 1 < c->in_end && c->in[i + 1] != '\n')) {
        refuse(c, 400);
        return -1;
      }
    }
    // A line that already ends badly need not wait for more.
    if (c->eof || c->error) {
      if (c->in_end > c->in_start) {
        refuse(c, 400);
      } else {
        settle_int(c, 0);
      }
      return -1;
    }
    return 0;
  }
  if ((size_t)end - c->in_start > HTTP_MAX_HEAD) {
    refuse(c, 431);
    return -1;
  }
  const uint8_t *s = c->in;
  size_t at = c->in_start;
  ptrdiff_t eol = line_end(s, at, (size_t)end + 2);
  if (eol < 0) {
    refuse(c, 400);
    return -1;
  }
  // METHOD SP target SP HTTP/1.1
  size_t line = (size_t)eol, i = at;
  while (i < line && is_token(s[i])) {
    ++i;
  }
  size_t method_end = i;
  if (method_end == at || i >= line || s[i] != ' ') {
    refuse(c, 400);
    return -1;
  }
  size_t target = ++i;
  while (i < line && s[i] > 0x20 && s[i] != 0x7F) {
    ++i;
  }
  size_t target_end = i;
  if (target_end == target || i >= line || s[i] != ' ') {
    refuse(c, 400);
    return -1;
  }
  int version = version_of(s + i + 1, line - i - 1);
  if (version <= 0) {
    refuse(c, version < 0 ? 505 : 400);
    return -1;
  }
  TzArray *headers = NULL;
  Framing f;
  if (!parse_fields(s, line + 2, (size_t)end, &headers, &f, 1)) {
    refuse(c, 400);
    return -1;
  }
  // The rules against smuggling: one way to tell where the body ends.
  int bad = (f.chunked || f.coded) && (f.length >= 0 || version == 10);
  if (version == 11 && f.hosts != 1) {
    bad = 1;
  }
  if (bad || f.coded) {
    if (tz_rc_dec(&headers->rc)) {
      tz_arr_free(headers);
    }
    refuse(c, bad ? 400 : 501);
    return -1;
  }
  if (h->max_body > 0 && f.length >= 0 && (double)f.length > h->max_body) {
    if (tz_rc_dec(&headers->rc)) {
      tz_arr_free(headers);
    }
    refuse(c, 413);
    return -1;
  }
  clear_head(h);
  h->method = latin1(s + at, method_end - at);
  h->target = latin1(s + target, target_end - target);
  h->headers = headers;
  h->version = version;
  h->keep_alive = (version == 11 ? !f.close : f.keep_alive && !f.close) && !h->ending;
  h->head_request = equals_lower(s + at, method_end - at, "head");
  h->body = f.chunked ? BODY_CHUNKED : f.length > 0 ? BODY_LENGTH : BODY_NONE;
  h->remaining = f.length > 0 ? f.length : 0;
  h->chunk_state = CHUNK_SIZE;
  h->body_done = h->body == BODY_NONE;
  h->body_read = 0;
  h->expect_continue = f.expect_continue && !h->body_done;
  h->continue_sent = 0;
  c->in_start = (uint32_t)end + 2;
  c->busy = 1;

  return 1;
}

// Reads the head of a response, after the 1xx ones; as read_request.
static int read_response(NetConn *c) {
  HttpState *h = &c->http;
  for (;;) {
    ptrdiff_t end = head_end(c);
    if (end < 0) {
      if (c->in_end - c->in_start > HTTP_MAX_HEAD) {
        settle_error(c, net_error(TZ_ERROR, "the head of the response is too large", NULL));
        net_abort(c);
        return -1;
      }
      if (c->eof || c->error) {
        h->keep_alive = 0;
        settle_error(c, broken_error(c));
        return -1;
      }
      return 0;
    }
    const uint8_t *s = c->in;
    size_t at = c->in_start;
    ptrdiff_t eol = line_end(s, at, (size_t)end + 2);
    int version = eol > 0 && (size_t)eol - at >= 12 ? version_of(s + at, 8) : 0;
    size_t line = eol > 0 ? (size_t)eol : at;
    int status = 0;
    if (version > 0 && s[at + 8] == ' ') {
      for (size_t k = at + 9; k < at + 12; ++k) {
        status = s[k] >= '0' && s[k] <= '9' ? status * 10 + (s[k] - '0') : -1000;
      }
    }
    if (version <= 0 || status < 100 || status > 999 || (line > at + 12 && s[at + 12] != ' ')) {
      settle_error(c, net_error(TZ_ERROR, "the response is not HTTP/1.1", NULL));
      net_abort(c);
      return -1;
    }
    TzArray *headers = NULL;
    Framing f;
    if (!parse_fields(s, line + 2, (size_t)end, &headers, &f, 0)) {
      settle_error(c, net_error(TZ_ERROR, "the response has a malformed header", NULL));
      net_abort(c);
      return -1;
    }
    c->in_start = (uint32_t)end + 2;
    if (status < 200 && status != 101) { // 100 Continue, 103 Early Hints
      if (tz_rc_dec(&headers->rc)) {
        tz_arr_free(headers);
      }
      continue;
    }
    if ((f.chunked || f.coded) && f.length >= 0) {
      if (tz_rc_dec(&headers->rc)) {
        tz_arr_free(headers);
      }
      settle_error(c, net_error(TZ_ERROR,
                                "the response has both Content-Length and "
                                "Transfer-Encoding",
                                NULL));
      net_abort(c);
      return -1;
    }
    clear_head(h);
    size_t reason = line > at + 13 ? at + 13 : line;
    h->status_text = latin1(s + reason, line - reason);
    h->headers = headers;
    h->status = status;
    h->version = version;
    h->keep_alive = version == 11 ? !f.close : f.keep_alive && !f.close;
    int none = h->head_request || status == 204 || status == 304 || status == 101;
    h->body = none            ? BODY_NONE
              : f.chunked     ? BODY_CHUNKED
              : f.length >= 0 ? (f.length > 0 ? BODY_LENGTH : BODY_NONE)
                              : BODY_TO_END;
    if (h->body == BODY_TO_END || f.coded) {
      h->keep_alive = 0;
    }
    if (f.coded && !f.chunked) {
      h->body = BODY_TO_END;
    }
    h->remaining = f.length > 0 ? f.length : 0;
    h->chunk_state = CHUNK_SIZE;
    h->body_done = h->body == BODY_NONE;
    h->body_read = 0;
    return 1;
  }
}

//===----------------------------------------------------------------------===//
// Bodies
//===----------------------------------------------------------------------===//

// Takes the next piece of the body from the input: 1 with `*data` and
// `*length` (in the input, which the caller copies before reading more), 0
// if more input is needed, 2 at the end of the body, -1 if it is malformed.
static int take_body(NetConn *c, const uint8_t **data, size_t *length) {
  HttpState *h = &c->http;
  for (;;) {
    if (h->body_done) {
      return 2;
    }
    size_t have = c->in_end - c->in_start;
    const uint8_t *s = c->in + c->in_start;
    switch (h->body) {
    case BODY_NONE:
      h->body_done = 1;
      return 2;
    case BODY_TO_END:
      if (have) {
        *data = s;
        *length = have;
        c->in_start = c->in_end;
        return 1;
      }
      if (c->eof) {
        h->body_done = 1;
        return 2;
      }
      return c->error ? -1 : 0;
    case BODY_LENGTH:
      if (h->remaining == 0) {
        h->body_done = 1;
        return 2;
      }
      if (!have) {
        return c->eof || c->error ? -1 : 0;
      }
      *length = have < (uint64_t)h->remaining ? have : (size_t)h->remaining;
      *data = s;
      c->in_start += (uint32_t)*length;
      h->remaining -= (int64_t)*length;
      if (h->remaining == 0) {
        h->body_done = 1;
      }
      return 1;
    case BODY_CHUNKED:
      switch (h->chunk_state) {
      case CHUNK_SIZE: {
        ptrdiff_t eol = line_end(c->in, c->in_start, c->in_end);
        if (eol == -2) {
          return -1;
        }
        if (eol < 0) {
          return have > HTTP_MAX_LINE || c->eof || c->error ? -1 : 0;
        }
        size_t i = c->in_start, end = (size_t)eol;
        int64_t size = 0;
        size_t digits = 0;
        while (i < end) {
          uint8_t d = c->in[i];
          int v = d >= '0' && d <= '9'   ? d - '0'
                  : d >= 'a' && d <= 'f' ? d - 'a' + 10
                  : d >= 'A' && d <= 'F' ? d - 'A' + 10
                                         : -1;
          if (v < 0) {
            break;
          }
          if (++digits > 15) {
            return -1;
          }
          size = size * 16 + v;
          ++i;
        }
        if (!digits) {
          return -1;
        }
        // Extensions after ';', which mean nothing here.
        while (i < end && is_space(c->in[i])) {
          ++i;
        }
        if (i < end && c->in[i] != ';') {
          return -1;
        }
        for (; i < end; ++i) {
          if (!is_value(c->in[i])) {
            return -1;
          }
        }
        c->in_start = (uint32_t)end + 2;
        h->remaining = size;
        h->chunk_state = size ? CHUNK_DATA : CHUNK_TRAILER;
        continue;
      }
      case CHUNK_DATA:
        if (!have) {
          return c->eof || c->error ? -1 : 0;
        }
        *length = have < (uint64_t)h->remaining ? have : (size_t)h->remaining;
        *data = s;
        c->in_start += (uint32_t)*length;
        h->remaining -= (int64_t)*length;
        if (h->remaining == 0) {
          h->chunk_state = CHUNK_DATA_END;
        }
        return 1;
      case CHUNK_DATA_END:
        if (have < 2) {
          return c->eof || c->error ? -1 : 0;
        }
        if (s[0] != '\r' || s[1] != '\n') {
          return -1;
        }
        c->in_start += 2;
        h->chunk_state = CHUNK_SIZE;
        continue;
      case CHUNK_TRAILER: {
        // Fields of the trailer, which are dropped, up to an empty line.
        ptrdiff_t eol = line_end(c->in, c->in_start, c->in_end);
        if (eol == -2) {
          return -1;
        }
        if (eol < 0) {
          return have > HTTP_MAX_HEAD || c->eof || c->error ? -1 : 0;
        }
        size_t start = c->in_start;
        c->in_start = (uint32_t)eol + 2;
        if ((size_t)eol == start) {
          h->body_done = 1;
          return 2;
        }
        size_t colon = start;
        while (colon < (size_t)eol && is_token(c->in[colon])) {
          ++colon;
        }
        if (colon == start || colon == (size_t)eol || c->in[colon] != ':') {
          return -1;
        }
        continue;
      }
      }
      return -1;
    }
    return -1;
  }
}

// What breaks a body, for the promise that waits for it.
static void *body_error(NetConn *c) {
  if (c->server) {
    return net_error(TZ_TYPE_ERROR, "the body of the request is malformed or was cut off", NULL);
  }
  void *cause = broken_error(c);

  return net_error(TZ_TYPE_ERROR, "terminated", cause);
}

static void *too_large(void) {
  return net_error(TZ_RANGE_ERROR, "the body of the request is larger than maxBodySize", NULL);
}

// 100 Continue, once the program reads the body of a request that asked.
static void send_continue(NetConn *c) {
  HttpState *h = &c->http;
  if (!h->expect_continue || h->continue_sent) {
    return;
  }
  h->continue_sent = 1;
  static const char text[] = "HTTP/1.1 100 Continue\r\n\r\n";
  net_write(c, text, sizeof text - 1);
  net_flush(c);
}

// The next piece of the body for the program waiting for it; or the whole
// body.
static void feed_body(NetConn *c) {
  HttpState *h = &c->http;
  for (;;) {
    const uint8_t *data;
    size_t length;
    int got = take_body(c, &data, &length);
    if (got == 0) {
      net_read(c);
      return;
    }
    if (got < 0) {
      // A server answers a malformed body itself, as node does: its handler
      // gets the error and what it answers goes.
      if (c->server && !c->eof && !c->error) {
        answer_error(c, 400);
      } else {
        net_abort(c);
      }
      h->keep_alive = 0;
      settle_error(c, body_error(c));
      return;
    }
    if (got == 1) {
      h->body_read += (int64_t)length;
      if (c->server && h->max_body > 0 && (double)h->body_read > h->max_body) {
        answer_error(c, 413);
        settle_error(c, too_large());
        return;
      }
    }
    if (h->wait_kind == WAIT_CHUNK) {
      TzBytes *chunk = got == 1 ? net_bytes(data, length) : NULL;
      settle_value(c, &chunk, sizeof chunk);
      return;
    }
    // The whole body.
    if (got == 1) {
      if (h->all_length + length > h->all_capacity) {
        size_t cap = h->all_capacity * 2 + length + 4096;
        uint8_t *all = (uint8_t *)tz_alloc(cap);
        if (h->all_length) {
          memcpy(all, h->all, h->all_length);
        }
        tz_free(h->all);
        h->all = all;
        h->all_capacity = cap;
      }
      memcpy(h->all + h->all_length, data, length);
      h->all_length += length;
      continue;
    }
    TzBytes *all = net_bytes(h->all, h->all_length);
    tz_free(h->all);
    h->all = NULL;
    h->all_length = h->all_capacity = 0;
    settle_value(c, &all, sizeof all);
    return;
  }
}

// Drops the rest of a body the program did not read, before the next
// message; false if that is too much to wait for (or broken), and the
// connection is not kept.
static int discard_body(NetConn *c, int *wait) {
  HttpState *h = &c->http;
  *wait = 0;
  while (!h->body_done) {
    const uint8_t *data;
    size_t length;
    int got = take_body(c, &data, &length);
    if (got == 0) {
      if (h->body_read > HTTP_MAX_DISCARD) {
        return 0;
      }
      *wait = 1;
      return 1;
    }
    if (got < 0) {
      return 0;
    }
    if (got == 1) {
      h->body_read += (int64_t)length;
    }
  }

  return 1;
}

//===----------------------------------------------------------------------===//
// What the program waits for
//===----------------------------------------------------------------------===//

static void input(NetConn *c) {
  HttpState *h = &c->http;
  switch (h->wait_kind) {
  case WAIT_REQUEST: {
    int wait = 0;
    if (!h->keep_alive || !discard_body(c, &wait)) {
      net_close(c);
      settle_int(c, 0);
      return;
    }
    if (wait) {
      net_read(c);
      return;
    }
    int got = read_request(c);
    if (got == 0) {
      net_read(c);
      return;
    }
    if (got > 0) {
      settle_int(c, 1);
    }
    return;
  }
  case WAIT_RESPONSE: {
    int got = read_response(c);
    if (got == 0) {
      net_read(c);
      return;
    }
    if (got > 0) {
      settle_int(c, h->status);
    }
    return;
  }
  case WAIT_CHUNK:
  case WAIT_ALL:
    feed_body(c);
    return;
  default:
    return;
  }
}

int tz_http_waits_request(NetConn *c) {
  return c->on_input == input && c->http.wait_kind == WAIT_REQUEST && c->http.waiting;
}

// `__tz_http_next(conn)`: the next request of a server's connection: 1 when
// its head is read, 0 when the connection ends (or a request was refused).
// It needs no turn: it touches only the connection, under its lock, which
// the await that follows lets go of once it waits (async.c), so that the
// promise cannot settle on another thread before (net_completed_pooled).
void tz_http_next(TzPromise *p, void *conn, TzMaker make) {
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  c->busy = 0;
  if (!c->http.version) { // the first request
    c->http.keep_alive = !c->http.ending;
  }
  if (begin_wait(c, p, WAIT_REQUEST, NULL, make)) {
    input(c);
  }
#if TZ_PARALLEL
  tz_thread.await_lock = &c->lock;
#else
  conn_unlock(c);
#endif
}

// `__tz_http_response(conn, signal)`: the status of the response, once its
// head is read.
void tz_http_response(TzPromise *p, void *conn, void *signal, TzMaker make) {
  TZ_ORDERED();
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  if (begin_wait(c, p, WAIT_RESPONSE, signal, make)) {
    input(c);
  }
  conn_unlock(c);
}

// `__tz_http_read(conn, signal)`: the next piece of the body, or undefined
// at its end.
void tz_http_read(TzPromise *p, void *conn, void *signal, TzMaker make) {
  TZ_ORDERED();
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  if (begin_wait(c, p, WAIT_CHUNK, signal, make)) {
    send_continue(c);
    input(c);
  }
  conn_unlock(c);
}

// `__tz_http_readAll(conn, signal)`: the rest of the body at once.
void tz_http_read_all(TzPromise *p, void *conn, void *signal, TzMaker make) {
  TZ_ORDERED();
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  if (begin_wait(c, p, WAIT_ALL, signal, make)) {
    send_continue(c);
    input(c);
  }
  conn_unlock(c);
}

// The parts of the head last read, each taken once, by the task of the
// connection: the connection lets go of them, so that what reads the next
// head (on another thread, maybe) never counts them at the same time.
static TzStr take_str(TzStr *s) {
  TzStr taken = *s;
  memset(s, 0, sizeof *s);

  return taken;
}

void tz_http_method(TzStr *out, void *conn, TzMaker make) {
  (void)make;
  *out = take_str(&((NetConn *)conn)->http.method);
}

void tz_http_target(TzStr *out, void *conn, TzMaker make) {
  (void)make;
  *out = take_str(&((NetConn *)conn)->http.target);
}

void tz_http_status_text(TzStr *out, void *conn, TzMaker make) {
  (void)make;
  *out = take_str(&((NetConn *)conn)->http.status_text);
}

void *tz_http_headers(void *conn, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;
  TzArray *headers = c->http.headers;
  if (!headers) {
    return tz_arr_new(&tz_string_type, 0, 0);
  }
  c->http.headers = NULL;

  return headers;
}

// Whether the message has a body (a length above zero, chunks, or up to the
// end of the connection).
int32_t tz_http_has_body(void *conn, TzMaker make) {
  (void)make;

  return ((NetConn *)conn)->http.body != BODY_NONE;
}

// Whether the connection can carry the next message: kept alive, the body
// read to its end, nothing broken.
int32_t tz_http_reusable(void *conn, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  int32_t reusable =
      c->http.keep_alive && c->http.body_done && !c->closed && !c->error && !c->eof && !c->closing;
  conn_unlock(c);

  return reusable;
}

int32_t tz_http_keep_alive(void *conn, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  int32_t keep = c->http.keep_alive;
  conn_unlock(c);

  return keep;
}

// The connection ends after the response being made.
void tz_http_end_after(void *conn, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  c->http.keep_alive = 0;
  conn_unlock(c);
}

//===----------------------------------------------------------------------===//
// Writing
//===----------------------------------------------------------------------===//

// The Date header, "Sat, 24 Oct 2026 10:00:00 GMT".
static const char *http_date(void) {
  time_t now = time(NULL);
  if ((int64_t)now != tz_http_date_second) {
    struct tm t;
#ifdef _WIN32
    gmtime_s(&t, &now);
#else
    gmtime_r(&now, &t);
#endif
    strftime(tz_http_date, sizeof tz_http_date, "%a, %d %b %Y %H:%M:%S GMT", &t);
    tz_http_date_second = (int64_t)now;
  }

  return tz_http_date;
}

// The UTF-8 of a string: its length, and the bytes written at `out` (when
// not NULL).
static size_t utf8_of(TzStr text, uint8_t *out) {
  uint32_t n;
  int two;
  const void *units = tz_str_units(&text, &n, &two);
  size_t length = 0;
  if (!two) {
    const uint8_t *s = (const uint8_t *)units;
    for (uint32_t i = 0; i < n; ++i) {
      if (s[i] < 0x80) {
        if (out) {
          out[length] = s[i];
        }
        ++length;
      } else {
        if (out) {
          out[length] = (uint8_t)(0xC0 | (s[i] >> 6));
          out[length + 1] = (uint8_t)(0x80 | (s[i] & 0x3F));
        }
        length += 2;
      }
    }
    return length;
  }
  const uint16_t *s = (const uint16_t *)units;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t c = s[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      c = 0xFFFD;
    }
    uint8_t buf[4];
    size_t k;
    if (c < 0x80) {
      buf[0] = (uint8_t)c;
      k = 1;
    } else if (c < 0x800) {
      buf[0] = (uint8_t)(0xC0 | (c >> 6));
      buf[1] = (uint8_t)(0x80 | (c & 0x3F));
      k = 2;
    } else if (c < 0x10000) {
      buf[0] = (uint8_t)(0xE0 | (c >> 12));
      buf[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
      buf[2] = (uint8_t)(0x80 | (c & 0x3F));
      k = 3;
    } else {
      buf[0] = (uint8_t)(0xF0 | (c >> 18));
      buf[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
      buf[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
      buf[3] = (uint8_t)(0x80 | (c & 0x3F));
      k = 4;
    }
    if (out) {
      memcpy(out + length, buf, k);
    }
    length += k;
  }

  return length;
}

// A header name or value as bytes (latin1, as node writes them): false if it
// has a character that does not fit a head (CR, LF, a zero, above 0xFF).
static int head_bytes(TzStr text, uint8_t *out, size_t *length) {
  uint32_t n;
  int two;
  const void *units = tz_str_units(&text, &n, &two);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t c = two ? ((const uint16_t *)units)[i] : ((const uint8_t *)units)[i];
    if (c == '\r' || c == '\n' || c == 0 || c > 0xFF) {
      return 0;
    }
    if (out) {
      out[i] = (uint8_t)c;
    }
  }
  *length = n;

  return 1;
}

static void put(NetConn *c, const char *text) { net_write(c, text, strlen(text)); }

static void put_str(NetConn *c, TzStr text) {
  size_t n;
  if (!head_bytes(text, NULL, &n)) {
    return;
  }
  uint8_t *at = net_reserve(c, n);
  head_bytes(text, at, &n);
  net_wrote(c, n);
}

// The headers of the program, but those of the framing, which this file
// writes; whether it gave Date and Content-Length (its value then in
// `*length`).
static void put_headers(NetConn *c, const TzArray *headers, int *date, int64_t *length) {
  *date = 0;
  const TzStr *items = (const TzStr *)headers->data;
  for (uint32_t i = 0; i + 1 < headers->length; i += 2) {
    TzStr name = items[i], value = items[i + 1];
    size_t n, m;
    char lowered_name[32];
    if (!head_bytes(name, NULL, &n) || !head_bytes(value, NULL, &m) || n == 0) {
      continue;
    }
    if (n < sizeof lowered_name) {
      head_bytes(name, (uint8_t *)lowered_name, &n);
      for (size_t k = 0; k < n; ++k) {
        lowered_name[k] = (char)lower((uint8_t)lowered_name[k]);
      }
      lowered_name[n] = 0;
      if (strcmp(lowered_name, "transfer-encoding") == 0 ||
          strcmp(lowered_name, "connection") == 0 || strcmp(lowered_name, "keep-alive") == 0) {
        continue;
      }
      if (strcmp(lowered_name, "content-length") == 0) {
        uint8_t digits[24];
        size_t d;
        if (m < sizeof digits && head_bytes(value, digits, &d)) {
          int64_t parsed;
          if (content_length(digits, d, &parsed)) {
            *length = parsed;
          }
        }
        continue;
      }
      if (strcmp(lowered_name, "date") == 0) {
        *date = 1;
      }
    }
    put_str(c, name);
    put(c, ": ");
    put_str(c, value);
    put(c, "\r\n");
  }
}

// The body as it is sent, from `flags` of tz_http_respond.
enum {
  RESPOND_STREAM = 1, // pieces follow (tz_http_write, tz_http_end)
  RESPOND_CLOSE = 2,  // the connection ends after it
  RESPOND_NO_BODY = 4,
  RESPOND_BYTES = 8, // the body is the bytes, not the text
};

static void respond(NetConn *c, int32_t status, void *text_obj, uint64_t text_meta, void *headers,
                    void *body_obj, uint64_t body_meta, void *bytes, int32_t flags);

// `__tz_http_respond(conn, status, statusText, headers, text, bytes,
// flags)`: the head and the body of a response, in one write. Without the
// turn: it touches only the connection, under its lock.
void tz_http_respond(void *conn, int32_t status, void *text_obj, uint64_t text_meta, void *headers,
                     void *body_obj, uint64_t body_meta, void *bytes, int32_t flags, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  respond(c, status, text_obj, text_meta, headers, body_obj, body_meta, bytes, flags);
  conn_unlock(c);
}

static void respond(NetConn *c, int32_t status, void *text_obj, uint64_t text_meta, void *headers,
                    void *body_obj, uint64_t body_meta, void *bytes, int32_t flags) {
  HttpState *h = &c->http;
  if (c->closed || c->error || h->refused) {
    return;
  }
  if (flags & RESPOND_CLOSE) {
    h->keep_alive = 0;
  }
  // A body the program did not read (or a request that asked to send one
  // first) keeps the connection only if it is small.
  if (h->expect_continue && !h->continue_sent) {
    h->keep_alive = 0;
  }
  char line[160];
  TzStr status_text = {text_obj, text_meta};
  uint32_t text_length;
  int two;
  tz_str_units(&status_text, &text_length, &two);
  if (text_length) {
    snprintf(line, sizeof line, "HTTP/1.1 %d ", status);
    put(c, line);
    put_str(c, status_text);
    put(c, "\r\n");
  } else {
    snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\n", status, reason_of(status));
    put(c, line);
  }
  int date;
  int64_t given = -1;
  put_headers(c, (const TzArray *)headers, &date, &given);
  if (!date) {
    put(c, "date: ");
    put(c, http_date());
    put(c, "\r\n");
  }
  int no_body = (flags & RESPOND_NO_BODY) || status == 204 || status == 304 ||
                (status >= 100 && status < 200);
  TzStr body = {body_obj, body_meta};
  const TzBytes *data = (const TzBytes *)bytes;
  size_t length = 0;
  if (!(flags & RESPOND_STREAM)) {
    length = (flags & RESPOND_BYTES) ? (data ? (size_t)tz_bytes_byte_length(data) : 0)
                                     : utf8_of(body, NULL);
  }
  if (flags & RESPOND_STREAM) {
    if (given >= 0) {
      h->response_mode = SEND_RAW;
      snprintf(line, sizeof line, "content-length: %lld\r\n", (long long)given);
      put(c, line);
    } else if (h->version == 11) {
      h->response_mode = SEND_CHUNKED;
      put(c, "transfer-encoding: chunked\r\n");
    } else {
      h->response_mode = SEND_RAW;
      h->keep_alive = 0;
    }
  } else if (status != 204 && status != 304 && !(status >= 100 && status < 200)) {
    // HEAD tells the length the body would have had.
    snprintf(
        line, sizeof line, "content-length: %llu\r\n",
        (unsigned long long)((flags & RESPOND_NO_BODY) && given >= 0 ? (size_t)given : length));
    put(c, line);
  }
  if (!h->keep_alive) {
    put(c, "connection: close\r\n");
  } else if (h->version == 10) {
    put(c, "connection: keep-alive\r\n");
  }
  put(c, "\r\n");
  if (!no_body && !(flags & RESPOND_STREAM) && length) {
    uint8_t *at = net_reserve(c, length);
    if (flags & RESPOND_BYTES) {
      memcpy(at, data->data, length);
    } else {
      utf8_of(body, at);
    }
    net_wrote(c, length);
  }
  if (no_body && (flags & RESPOND_STREAM)) {
    h->response_mode = SEND_RAW;
  }
  net_flush(c);
  if (!(flags & RESPOND_STREAM) && !h->keep_alive) {
    net_close(c);
  }
}

// `__tz_http_request(conn, method, target, headers, text, bytes, flags)`:
// the head and the body of a request, in one write. The headers have Host.
static void request(NetConn *c, void *method_obj, uint64_t method_meta, void *target_obj,
                    uint64_t target_meta, void *headers, void *body_obj, uint64_t body_meta,
                    void *bytes, int32_t flags);

void tz_http_request(void *conn, void *method_obj, uint64_t method_meta, void *target_obj,
                     uint64_t target_meta, void *headers, void *body_obj, uint64_t body_meta,
                     void *bytes, int32_t flags, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  request(c, method_obj, method_meta, target_obj, target_meta, headers, body_obj, body_meta, bytes,
          flags);
  conn_unlock(c);
}

static void request(NetConn *c, void *method_obj, uint64_t method_meta, void *target_obj,
                    uint64_t target_meta, void *headers, void *body_obj, uint64_t body_meta,
                    void *bytes, int32_t flags) {
  HttpState *h = &c->http;
  TzStr method = {method_obj, method_meta}, target = {target_obj, target_meta};
  put_str(c, method);
  put(c, " ");
  put_str(c, target);
  put(c, " HTTP/1.1\r\n");
  int date;
  int64_t given = -1;
  put_headers(c, (const TzArray *)headers, &date, &given);
  TzStr body = {body_obj, body_meta};
  const TzBytes *data = (const TzBytes *)bytes;
  size_t length = 0;
  if (flags & RESPOND_STREAM) {
    h->response_mode = SEND_CHUNKED;
    put(c, "transfer-encoding: chunked\r\n");
  } else if (!(flags & RESPOND_NO_BODY)) {
    length = (flags & RESPOND_BYTES) ? (data ? (size_t)tz_bytes_byte_length(data) : 0)
                                     : utf8_of(body, NULL);
    char line[64];
    snprintf(line, sizeof line, "content-length: %llu\r\n", (unsigned long long)length);
    put(c, line);
  }
  put(c, "\r\n");
  if (length) {
    uint8_t *at = net_reserve(c, length);
    if (flags & RESPOND_BYTES) {
      memcpy(at, data->data, length);
    } else {
      utf8_of(body, at);
    }
    net_wrote(c, length);
  }
  // What the response to it will be read as.
  uint32_t n;
  int two;
  const void *units = tz_str_units(&method, &n, &two);
  h->head_request = !two && n == 4 && memcmp(units, "HEAD", 4) == 0;
  h->keep_alive = 1;
  net_flush(c);
}

// `__tz_http_write(conn, chunk)`: a piece of a body that streams; waits
// while too much is queued.
static void write_chunk(TzPromise *p, NetConn *c, void *bytes, TzMaker make);

void tz_http_write(TzPromise *p, void *conn, void *bytes, TzMaker make) {
  TZ_ORDERED();
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  write_chunk(p, c, bytes, make);
  conn_unlock(c);
}

static void write_chunk(TzPromise *p, NetConn *c, void *bytes, TzMaker make) {
  const TzBytes *data = (const TzBytes *)bytes;
  size_t length = (size_t)tz_bytes_byte_length(data);
  if (c->closed || c->error || c->http.refused) {
    void *error = net_error(TZ_ERROR, "the connection is closed", NULL);
    tz_promise_reject(p, error);
    return;
  }
  if (length) {
    if (c->http.response_mode == SEND_CHUNKED) {
      char line[32];
      int n = snprintf(line, sizeof line, "%llx\r\n", (unsigned long long)length);
      net_write(c, line, (size_t)n);
      net_write(c, data->data, length);
      net_write(c, "\r\n", 2);
    } else {
      net_write(c, data->data, length);
    }
    net_flush(c);
  }
  tz_net_make = make;
  tz_ref_retain(p); // it may be a member of a group (async.c)
  if (c->drain) {
    // One waits at a time; the program awaits each write.
    TzPromise *old = c->drain;
    c->drain = NULL;
    net_fulfill(&old, NULL, 0);
  }
  c->drain = p;
  if (net_pending_output(c) < NET_HIGH_WATER_MARK || c->closed || c->error) {
    net_fulfill(&c->drain, NULL, 0);
  }
}

// `__tz_http_end(conn)`: the end of a body that streams.
void tz_http_end(void *conn, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  if (!c->closed && !c->error && !c->http.refused) {
    if (c->http.response_mode == SEND_CHUNKED) {
      net_write(c, "0\r\n\r\n", 5);
    }
    net_flush(c);
    if (c->server && !c->http.keep_alive) {
      net_close(c);
    }
  }
  conn_unlock(c);
}
