#ifdef _WIN32
#define _CRT_RAND_S
#endif
#include "runtime/library/net/net/net.h"
#include "runtime/library/http/http2/http2.h"
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
#include "runtime/library/net/deadline/deadline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <poll.h>
#endif

#define HTTP_MAX_HEAD 16384
#define HTTP_MAX_HEADERS 1000
#define HTTP_MAX_LINE 4096
#define HTTP_MAX_DISCARD 1048576

enum { WAIT_NONE, WAIT_REQUEST, WAIT_RESPONSE, WAIT_CHUNK, WAIT_ALL, WAIT_DRAIN, WAIT_RAW };

enum { BODY_NONE, BODY_LENGTH, BODY_CHUNKED, BODY_TO_END };

enum { CHUNK_SIZE, CHUNK_DATA, CHUNK_DATA_END, CHUNK_TRAILER };

enum { SEND_FIXED, SEND_CHUNKED, SEND_RAW };

TZ_THREAD_LOCAL char tz_http_date[32];
TZ_THREAD_LOCAL int64_t tz_http_date_second = -1;

static TzStr ascii(const char *text) { return tz_str_from_ascii(text, strlen(text)); }

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
  if (h->deadline) {
    tz_deadline_unlisten(&h->abort);
    tz_instance_release((TzInstance *)h->deadline);
    h->deadline = NULL;
  }
}

void tz_http_release(NetConn *c) {
  HttpState *h = &c->http;
  if (c->h2parent) {
    tz_h2_dropped(c);
  } else if (c->h2) {
    tz_h2_release(c);
  }
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

static void settle_int(NetConn *c, int64_t value) { settle_value(c, &value, sizeof value); }

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

static void *broken_error(NetConn *c) {
  if (c->error == H2_REFUSED) {
    return net_failure("ERR_NOT_PROCESSED",
                       "the server did not process the request, so it can be sent again");
  }
  if (c->error == H2_RESET) {
    return net_failure("ERR_STREAM_RESET", "the server reset the stream of the request");
  }
  if (c->error == NET_TLS_FAILED) {
    return c->tls_ops->error(c, c->http.make);
  }
  if (c->error && c->error != -1) {
    return net_system_error(c->http.make, c->error, "read", NULL, 0);
  }

  return net_failure("ERR_CLOSED", "other side closed");
}

static void input(NetConn *c);
static void raw_input(NetConn *c);

static void abort_wait(TzDeadlineListener *l) {
  HttpState *h = CONTAINING_RECORD(l, HttpState, abort);
  NetConn *c = CONTAINING_RECORD(h, NetConn, http);
  conn_lock(c);
  TzDeadline *deadline = h->deadline;
  h->deadline = NULL;
  void *reason = net_timed_out();
  tz_instance_release((TzInstance *)deadline);
  h->wait_kind = WAIT_NONE;
  if (h->waiting) {
    net_reject(&h->waiting, reason);
  } else {
    tz_instance_release((TzInstance *)reason);
  }
  h->keep_alive = 0;
  net_abort(c);
  conn_unlock(c);
}

static int begin_wait(NetConn *c, TzPromise *p, int kind, void *deadline, TzMaker make) {
  HttpState *h = &c->http;
  tz_ref_retain(p);
  h->waiting = p;
  h->wait_kind = kind;
  h->make = make;
  tz_net_make = make;
  if (deadline) {
    if (tz_deadline_is_passed(deadline)) {
      h->wait_kind = WAIT_NONE;
      net_reject(&h->waiting, net_timed_out());
      h->keep_alive = 0;
      net_abort(c);
      return 0;
    }
    tz_ref_retain(deadline);
    h->deadline = (TzDeadline *)deadline;
    h->abort.run = abort_wait;
    tz_deadline_listen(h->deadline, &h->abort);
  }
  c->on_input = input;

  return 1;
}

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

typedef struct {
  int64_t length;
  int chunked;
  int coded;
  int close, keep_alive, hosts, expect_continue;
  const uint8_t *host;
  size_t host_length;
} Framing;

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
    while (i < n && v[i] != ',') {
      ++i;
    }
    if (!length) {
      continue;
    }
    if (f->chunked) {
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

static int is_digit(uint8_t c) { return c >= '0' && c <= '9'; }

static int hex_value(uint8_t c) {
  if (is_digit(c)) {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static int ipv4_number(const uint8_t *s, size_t n, uint64_t *out) {
  if (n == 0) {
    return 0;
  }
  int radix = 10;
  if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    radix = 16;
    s += 2;
    n -= 2;
  } else if (n >= 2 && s[0] == '0') {
    radix = 8;
    s += 1;
    n -= 1;
  }
  uint64_t value = 0;
  for (size_t i = 0; i < n; ++i) {
    int digit = hex_value(s[i]);
    if (digit < 0 || digit >= radix) {
      return 0;
    }
    value = value > (UINT64_C(1) << 40) ? value : value * (uint64_t)radix + (uint64_t)digit;
  }
  *out = value;
  return 1;
}

static size_t last_label(const uint8_t *s, size_t n, size_t *start) {
  if (n > 0 && s[n - 1] == '.') {
    --n;
  }
  size_t from = n;
  while (from > 0 && s[from - 1] != '.') {
    --from;
  }
  *start = from;
  return n;
}

static int ends_in_number(const uint8_t *s, size_t n) {
  size_t start;
  n = last_label(s, n, &start);
  const uint8_t *last = s + start;
  size_t length = n - start;
  int digits = length > 0;
  for (size_t i = 0; i < length; ++i) {
    digits &= is_digit(last[i]);
  }
  uint64_t value;
  return digits || ipv4_number(last, length, &value);
}

static int ipv4_ok(const uint8_t *s, size_t n) {
  size_t start;
  n = last_label(s, n, &start);
  uint64_t numbers[4];
  size_t count = 0, from = 0;
  for (size_t i = 0; i <= n; ++i) {
    if (i < n && s[i] != '.') {
      continue;
    }
    if (count == 4 || !ipv4_number(s + from, i - from, &numbers[count])) {
      return 0;
    }
    ++count;
    from = i + 1;
  }
  for (size_t i = 0; i + 1 < count; ++i) {
    if (numbers[i] > 255) {
      return 0;
    }
  }
  return numbers[count - 1] < (UINT64_C(1) << (8 * (5 - count)));
}

static int ipv6_ok(const uint8_t *s, size_t n) {
  size_t at = 0;
  int piece = 0, compressed = 0;
  if (n > 0 && s[0] == ':') {
    if (n < 2 || s[1] != ':') {
      return 0;
    }
    at = 2;
    compressed = 1;
    ++piece;
  }
  while (at < n) {
    if (piece == 8) {
      return 0;
    }
    if (s[at] == ':') {
      if (compressed) {
        return 0;
      }
      ++at;
      ++piece;
      compressed = 1;
      continue;
    }
    size_t length = 0;
    while (length < 4 && at < n && hex_value(s[at]) >= 0) {
      ++at;
      ++length;
    }
    if (at < n && s[at] == '.') {
      if (length == 0 || piece > 6) {
        return 0;
      }
      at -= length;
      int seen = 0;
      while (at < n) {
        if (seen > 0) {
          if (s[at] != '.' || seen >= 4) {
            return 0;
          }
          ++at;
        }
        if (at >= n || !is_digit(s[at])) {
          return 0;
        }
        int number = -1;
        while (at < n && is_digit(s[at])) {
          if (number == 0) {
            return 0;
          }
          number = number < 0 ? s[at] - '0' : number * 10 + (s[at] - '0');
          if (number > 255) {
            return 0;
          }
          ++at;
        }
        ++seen;
        if (seen == 2 || seen == 4) {
          ++piece;
        }
      }
      return seen == 4 && (compressed || piece == 8);
    }
    if (at < n && s[at] == ':') {
      ++at;
      if (at >= n) {
        return 0;
      }
    } else if (at < n) {
      return 0;
    }
    ++piece;
  }
  return compressed || piece == 8;
}

static int host_ok(const uint8_t *s, size_t n) {
  size_t host_end;
  if (n > 0 && s[0] == '[') {
    host_end = 1;
    while (host_end < n && s[host_end] != ']') {
      ++host_end;
    }
    if (host_end >= n || !ipv6_ok(s + 1, host_end - 1)) {
      return 0;
    }
    ++host_end;
  } else {
    host_end = 0;
    while (host_end < n && s[host_end] != ':') {
      uint8_t c = s[host_end];
      if (c <= 0x20 || c >= 0x7F || strchr("#%/<>?@[\\]^|", c)) {
        return 0;
      }
      ++host_end;
    }
    if (host_end == 0 || (ends_in_number(s, host_end) && !ipv4_ok(s, host_end))) {
      return 0;
    }
  }
  if (host_end == n) {
    return 1;
  }
  if (s[host_end] != ':') {
    return 0;
  }
  uint32_t port = 0;
  for (size_t i = host_end + 1; i < n; ++i) {
    if (!is_digit(s[i])) {
      return 0;
    }
    port = port > 65535 ? port : port * 10 + (uint32_t)(s[i] - '0');
  }
  return port <= 65535;
}

static int target_ok(const uint8_t *s, size_t n) {
  size_t skip = n >= 7 && memcmp(s, "http://", 7) == 0    ? 7
                : n >= 8 && memcmp(s, "https://", 8) == 0 ? 8
                                                          : 0;
  if (skip == 0) {
    return 1;
  }
  size_t end = skip;
  while (end < n && s[end] != '/' && s[end] != '?' && s[end] != '#' && s[end] != '\\') {
    ++end;
  }
  return host_ok(s + skip, end - skip);
}

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
      f->host = s + v;
      f->host_length = ve - v;
      if (server && !host_ok(s + v, ve - v)) {
        ok = 0;
        break;
      }
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
    f->coded = 1;
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

static ptrdiff_t head_end(NetConn *c) {
  const uint8_t *s = c->in;
  for (size_t i = c->in_start; i + 3 < c->in_end; ++i) {
    if (s[i] == '\r' && s[i + 1] == '\n' && s[i + 2] == '\r' && s[i + 3] == '\n') {
      return (ptrdiff_t)(i + 2);
    }
  }

  return -1;
}

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

void tz_http_reason(TzStr *out, int64_t status, TzMaker make) {
  (void)make;
  *out = ascii(reason_of(status));
}

static void answer_error(NetConn *c, int status) {
  if (c->h2parent) {
    c->http.keep_alive = 0;
    c->http.refused = 1;
    c->out_len = c->flight_len = 0;
    tz_h2_respond(c, status, NULL, 1, 0);
    return;
  }
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

static TzStr request_url(NetConn *c, const uint8_t *target, size_t n, const Framing *f) {
  if ((n >= 7 && memcmp(target, "http://", 7) == 0) ||
      (n >= 8 && memcmp(target, "https://", 8) == 0)) {
    return latin1(target, n);
  }
  NetListener *l = c->listener;
  const char *scheme = l && l->tls ? "https://" : "http://";
  TzBuf b;
  tz_buf_init(&b);
  tz_buf_bytes(&b, (const uint8_t *)scheme, strlen(scheme));
  if (target[0] == '/' && f->host) {
    tz_buf_bytes(&b, f->host, f->host_length);
  } else {
    const char *host = l ? l->host : "localhost";
    int bracket = strchr(host, ':') != NULL;
    char text[300];
    int length = snprintf(text, sizeof text, "%s%s%s:%d", bracket ? "[" : "", host,
                          bracket ? "]" : "", l ? (int)l->port : 80);
    tz_buf_bytes(&b, (const uint8_t *)text, (size_t)length);
    if (target[0] != '/') {
      tz_buf_bytes(&b, (const uint8_t *)"/", 1);
    }
  }
  tz_buf_bytes(&b, target, n);

  return tz_buf_take(&b);
}

static int read_request(NetConn *c) {
  HttpState *h = &c->http;
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
    if (c->in_end > c->in_start && c->in[c->in_start] != '\r' && !is_token(c->in[c->in_start])) {
      refuse(c, 400);
      return -1;
    }
    for (size_t i = c->in_start; i < c->in_end; ++i) {
      if ((c->in[i] == '\n' && (i == c->in_start || c->in[i - 1] != '\r')) ||
          (c->in[i] == '\r' && i + 1 < c->in_end && c->in[i + 1] != '\n')) {
        refuse(c, 400);
        return -1;
      }
    }
    if (c->eof || c->error) {
      if (c->in_end > c->in_start) {
        refuse(c, 400);
      } else {
        settle_int(c, 0);
      }
      return -1;
    }
    if (c->in_end > c->in_start && h->head_since == 0) {
      h->head_since = net_now();
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
  if (target_end == target || i >= line || s[i] != ' ' ||
      !target_ok(s + target, target_end - target)) {
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
  h->target = request_url(c, s + target, target_end - target, &f);
  h->request_since = h->head_since > 0 ? h->head_since : net_now();
  h->responded = 0;
  h->head_since = 0;
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

static const char h2_answer_hint[] =
    "the server answered in HTTP/2 without TLS (h2c); add httpVersion: \"2\" to fetch";

static int looks_like_h2(const NetConn *c) {
  const uint8_t *s = c->in + c->in_start;
  size_t have = c->in_end - c->in_start;

  return have >= 9 && (s[3] == 0x04 || s[3] == 0x07) && s[5] == 0 && s[6] == 0 && s[7] == 0 &&
         s[8] == 0;
}

static int read_response(NetConn *c) {
  HttpState *h = &c->http;
  for (;;) {
    ptrdiff_t end = head_end(c);
    if (end < 0 && looks_like_h2(c)) {
      settle_error(c, net_failure("ERR_INVALID_RESPONSE", h2_answer_hint));
      net_abort(c);
      return -1;
    }
    if (end < 0) {
      if (c->in_end - c->in_start > HTTP_MAX_HEAD) {
        settle_error(c, net_failure("ERR_HEAD_TOO_LARGE", "the head of the response is too large"));
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
      settle_error(c,
                   net_failure("ERR_INVALID_RESPONSE",
                               looks_like_h2(c) ? h2_answer_hint : "the response is not HTTP/1.1"));
      net_abort(c);
      return -1;
    }
    TzArray *headers = NULL;
    Framing f;
    if (!parse_fields(s, line + 2, (size_t)end, &headers, &f, 0)) {
      settle_error(c, net_failure("ERR_INVALID_RESPONSE", "the response has a malformed header"));
      net_abort(c);
      return -1;
    }
    c->in_start = (uint32_t)end + 2;
    if (status < 200 && status != 101) {
      if (tz_rc_dec(&headers->rc)) {
        tz_arr_free(headers);
      }
      continue;
    }
    if ((f.chunked || f.coded) && f.length >= 0) {
      if (tz_rc_dec(&headers->rc)) {
        tz_arr_free(headers);
      }
      settle_error(c, net_failure("ERR_INVALID_RESPONSE",
                                  "the response has both Content-Length and Transfer-Encoding"));
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
        if (c->h2parent) {
          tz_h2_consumed(c, have);
        }
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

static void *body_error(NetConn *c) {
  if (!c->eof && !c->error) {
    return net_failure("ERR_INVALID_CHUNKED", "the body is not valid chunked encoding");
  }
  if (c->server) {
    return net_failure("ERR_CLOSED", "the client closed the connection before the end of the body");
  }

  return broken_error(c);
}

static void *too_large(void) {
  return net_failure("ERR_BODY_TOO_LARGE", "the body of the request is larger than maxBodySize");
}

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

static int switch_to_h2(NetConn *c) {
  if (c->http.version || c->h2parent) {
    return 0;
  }
  const NetListener *l = c->listener;
  int only = l && l->h2only;
  int h2 = 0;
  if (c->tls) {
    h2 = c->tls_ops->h2 && c->tls_ops->h2(c);
  }
  if (!h2 && l && (only || (!c->tls && l->h2c))) {
    h2 = tz_h2_preface(c);
    if (h2 < 0 && !c->eof && !c->error && !c->closed) {
      net_read(c);
      return 1;
    }
    if (h2 < 0) {
      h2 = 0;
    }
  }
  if (!h2) {
    if (only) {
      refuse(c, 505);
      return 1;
    }
    return 0;
  }
  settle_int(c, 2);
  tz_h2_start(c);

  return 1;
}

static void input(NetConn *c) {
  HttpState *h = &c->http;
  switch (h->wait_kind) {
  case WAIT_REQUEST: {
    if (c->h2parent) {
      int fresh = c->h2fresh;
      c->h2fresh = 0;
      c->busy = fresh;
      settle_int(c, fresh);
      return;
    }
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
    if (switch_to_h2(c)) {
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
    if (c->h2parent) {
      if (c->h2fresh || tz_h2_head_done(c)) {
        c->h2fresh = 0;
        settle_int(c, h->status);
      } else if (c->error || c->closed) {
        settle_error(c, broken_error(c));
      }
      return;
    }
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
  case WAIT_RAW:
    raw_input(c);
    return;
  default:
    return;
  }
}

static void raw_input(NetConn *c) {
  size_t have = c->in_end - c->in_start;
  if (have) {
    TzBytes *chunk = net_bytes(c->in + c->in_start, have);
    c->in_start = c->in_end = 0;
    settle_value(c, &chunk, sizeof chunk);
    return;
  }
  if (c->error && c->error != -1) {
    settle_error(c, broken_error(c));
    return;
  }
  if (c->eof || c->closed) {
    TzBytes *none = NULL;
    settle_value(c, &none, sizeof none);
    return;
  }
  net_read(c);
}

void tz_tcp_read(TzPromise *p, void *conn, void *deadline, TzMaker make) {
  TZ_ORDERED();
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  if (begin_wait(c, p, WAIT_RAW, deadline, make)) {
    input(c);
  }
  conn_unlock(c);
}

void tz_tcp_no_delay(void *conn, int32_t on, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;
  BOOL value = on ? TRUE : FALSE;
  setsockopt(c->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&value, sizeof value);
}

void tz_tcp_keep_alive(void *conn, int32_t on, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;
  BOOL value = on ? TRUE : FALSE;
  setsockopt(c->s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&value, sizeof value);
}

void tz_ws_random(void *bytes, TzMaker make) {
  (void)make;
  TzBytes *b = (TzBytes *)bytes;
  size_t length = (size_t)tz_bytes_byte_length(b);
  uint8_t *data = (uint8_t *)b->data;
  for (size_t i = 0; i < length; i += 4) {
    unsigned int value = 0;
    if (rand_s(&value) != 0) {
      value = (unsigned int)GetTickCount() * 2654435761u;
    }
    for (size_t k = 0; k < 4 && i + k < length; ++k) {
      data[i + k] = (uint8_t)(value >> (k * 8));
    }
  }
}

void tz_tcp_reset(void *conn, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  if (!c->closed) {
    struct linger hard = {1, 0};
    setsockopt(c->s, SOL_SOCKET, SO_LINGER, (const char *)&hard, sizeof hard);
    net_abort(c);
  }
  conn_unlock(c);
}

void tz_http_too_slow(NetConn *c) {
  HttpState *h = &c->http;
  h->head_since = 0;
  if (h->wait_kind == WAIT_REQUEST) {
    refuse(c, 408);
  }
}

void tz_http_request_too_slow(NetConn *c) {
  HttpState *h = &c->http;
  h->request_since = 0;
  h->keep_alive = 0;
  if (h->wait_kind == WAIT_CHUNK || h->wait_kind == WAIT_ALL) {
    settle_error(c, net_failure("TIMEOUT_ERR", "the request did not come in full in time"));
  }
  if (h->responded) {
    net_abort(c);
    return;
  }
  answer_error(c, 408);
}

double tz_http_body_limit(void *conn, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;

  return c->http.max_body;
}

int64_t tz_http_request_timeout(void *conn, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;

  return (int64_t)c->http.request_timeout;
}

void tz_http_stream_input(NetConn *c) { input(c); }

void tz_http_stream_request(NetConn *s, TzStr method, TzStr target, TzArray *headers, int head,
                            int ended) {
  HttpState *h = &s->http;
  clear_head(h);
  h->method = method;
  h->target = target;
  h->headers = headers;
  h->version = 20;
  h->keep_alive = 0;
  h->head_request = (uint8_t)head;
  h->body = ended ? BODY_NONE : BODY_TO_END;
  h->body_done = (uint8_t)ended;
  h->body_read = 0;
  h->request_since = net_now();
  h->responded = 0;
  if (ended) {
    s->eof = 1;
  }
}

void tz_http_stream_response(NetConn *s, int32_t status, TzArray *headers, int ended) {
  HttpState *h = &s->http;
  clear_head(h);
  h->status = status;
  h->status_text = ascii(reason_of(status));
  h->headers = headers;
  h->version = 20;
  h->keep_alive = 0;
  int none = ended || h->head_request || status == 204 || status == 304;
  h->body = none ? BODY_NONE : BODY_TO_END;
  h->body_done = (uint8_t)none;
  h->body_read = 0;
  h->remaining = 0;
  if (ended) {
    s->eof = 1;
  }
}

int tz_http_waits_request(NetConn *c) {
  return c->on_input == input && c->http.wait_kind == WAIT_REQUEST && c->http.waiting;
}

void tz_http_next(TzPromise *p, void *conn, TzMaker make) {
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  c->busy = 0;
  if (!c->http.version) {
    c->http.keep_alive = !c->http.ending;
  }
  if (begin_wait(c, p, WAIT_REQUEST, NULL, make)) {
    input(c);
  }
#if TZ_PARALLEL
  tz_thread.await_lock = conn_lock_of(c);
#else
  conn_unlock(c);
#endif
}

void tz_http_response(TzPromise *p, void *conn, void *deadline, TzMaker make) {
  TZ_ORDERED();
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  if (begin_wait(c, p, WAIT_RESPONSE, deadline, make)) {
    input(c);
  }
  conn_unlock(c);
}

void tz_http_read(TzPromise *p, void *conn, void *deadline, TzMaker make) {
  TZ_ORDERED();
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  if (begin_wait(c, p, WAIT_CHUNK, deadline, make)) {
    send_continue(c);
    input(c);
  }
  if (c->h2parent) {
    tz_h2_send_pending(c);
  }
  conn_unlock(c);
}

void tz_http_read_all(TzPromise *p, void *conn, void *deadline, TzMaker make) {
  TZ_ORDERED();
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  if (begin_wait(c, p, WAIT_ALL, deadline, make)) {
    send_continue(c);
    input(c);
  }
  if (c->h2parent) {
    tz_h2_send_pending(c);
  }
  conn_unlock(c);
}

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

int32_t tz_http_has_body(void *conn, TzMaker make) {
  (void)make;

  return ((NetConn *)conn)->http.body != BODY_NONE;
}

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

int32_t tz_http_alive(void *conn, TzMaker make) {
  TZ_ORDERED();
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  int32_t alive = !c->closed && !c->error && !c->eof && !c->closing;
  if (alive && !c->reading) {
    if (c->in_end > c->in_start || c->raw_end > c->raw_start) {
      alive = 0;
    } else {
#ifdef _WIN32
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(c->s, &readable);
      TIMEVAL now = {0, 0};
      alive = select(0, &readable, NULL, NULL, &now) == 0;
#else
      struct pollfd watched = {c->s, POLLIN, 0};
      alive = poll(&watched, 1, 0) == 0;
#endif
    }
  }
  conn_unlock(c);

  return alive;
}

int32_t tz_http_keep_alive(void *conn, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  int32_t keep = c->http.keep_alive;
  conn_unlock(c);

  return keep;
}

void tz_http_end_after(void *conn, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  c->http.keep_alive = 0;
  conn_unlock(c);
}

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

const char *tz_http_date_text(void) { return http_date(); }

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

enum {
  RESPOND_STREAM = 1,
  RESPOND_CLOSE = 2,
  RESPOND_NO_BODY = 4,
  RESPOND_BYTES = 8,
};

static void respond(NetConn *c, int32_t status, void *text_obj, uint64_t text_meta, void *headers,
                    void *body_obj, uint64_t body_meta, void *bytes, int32_t flags);

void tz_http_respond(void *conn, int64_t status, void *text_obj, uint64_t text_meta, void *headers,
                     void *body_obj, uint64_t body_meta, void *bytes, int64_t flags, TzMaker make) {
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
  h->responded = 1;
  if (c->h2parent) {
    (void)text_obj;
    (void)text_meta;
    int stream = (flags & RESPOND_STREAM) != 0;
    int no_body = (flags & RESPOND_NO_BODY) ||
                  (!stream && (status == 204 || status == 304 || (status >= 100 && status < 200)));
    if (!no_body && !stream) {
      TzStr text = {body_obj, body_meta};
      const TzBytes *data = (const TzBytes *)bytes;
      size_t length = (flags & RESPOND_BYTES) ? (data ? (size_t)tz_bytes_byte_length(data) : 0)
                                              : utf8_of(text, NULL);
      if (length) {
        uint8_t *at = net_reserve(c, length);
        if (flags & RESPOND_BYTES) {
          memcpy(at, data->data, length);
        } else {
          utf8_of(text, at);
        }
        net_wrote(c, length);
      }
    }
    tz_h2_respond(c, status, (const TzArray *)headers, no_body, stream);
    return;
  }
  if (flags & RESPOND_CLOSE) {
    h->keep_alive = 0;
  }
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

static void request(NetConn *c, void *method_obj, uint64_t method_meta, void *target_obj,
                    uint64_t target_meta, void *headers, void *body_obj, uint64_t body_meta,
                    void *bytes, int32_t flags);

void tz_http_request(void *conn, void *method_obj, uint64_t method_meta, void *target_obj,
                     uint64_t target_meta, void *headers, void *body_obj, uint64_t body_meta,
                     void *bytes, int64_t flags, TzMaker make) {
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
  if (c->h2parent) {
    uint32_t units_length;
    int wide;
    const void *units = tz_str_units(&method, &units_length, &wide);
    h->head_request = !wide && units_length == 4 && memcmp(units, "HEAD", 4) == 0;
    int stream = (flags & RESPOND_STREAM) != 0;
    int no_body = (flags & RESPOND_NO_BODY) != 0;
    if (!no_body && !stream) {
      TzStr text = {body_obj, body_meta};
      const TzBytes *data = (const TzBytes *)bytes;
      size_t length = (flags & RESPOND_BYTES) ? (data ? (size_t)tz_bytes_byte_length(data) : 0)
                                              : utf8_of(text, NULL);
      if (length) {
        uint8_t *at = net_reserve(c, length);
        if (flags & RESPOND_BYTES) {
          memcpy(at, data->data, length);
        } else {
          utf8_of(text, at);
        }
        net_wrote(c, length);
      } else {
        no_body = 1;
      }
    }
    tz_h2_request(c, method, target, (const TzArray *)headers, no_body, stream);
    return;
  }
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
  uint32_t n;
  int two;
  const void *units = tz_str_units(&method, &n, &two);
  h->head_request = !two && n == 4 && memcmp(units, "HEAD", 4) == 0;
  h->keep_alive = 1;
  net_flush(c);
}

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
    tz_net_make = make;
    tz_promise_fail(p, net_failure("ERR_CLOSED", "the connection is closed"));
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
  tz_ref_retain(p);
  if (c->drain) {
    TzPromise *old = c->drain;
    c->drain = NULL;
    net_fulfill(&old, NULL, 0);
  }
  c->drain = p;
  if (net_pending_output(c) < NET_HIGH_WATER_MARK || c->closed || c->error) {
    net_fulfill(&c->drain, NULL, 0);
  }
}

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
    if (c->h2parent && !c->server) {
      tz_h2_end(c);
    } else if (c->server && !c->http.keep_alive) {
      net_close(c);
    }
  }
  conn_unlock(c);
}

static int stream_busy(TzPromise *p, NetConn *c, TzMaker make) {
  if (!c->http.waiting) {
    return 0;
  }
  tz_net_make = make;
  tz_promise_fail(p, net_failure("ERR_HTTP2_BUSY", "another task already waits on this stream"));

  return 1;
}

static void stream_await(NetConn *c) {
#if TZ_PARALLEL
  tz_thread.await_lock = conn_lock_of(c);
#else
  conn_unlock(c);
#endif
}

void tz_http2_read(TzPromise *p, void *conn, TzMaker make) {
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  if (!stream_busy(p, c, make) && begin_wait(c, p, WAIT_CHUNK, NULL, make)) {
    input(c);
    tz_h2_send_pending(c);
  }
  stream_await(c);
}

void tz_http2_response(TzPromise *p, void *conn, TzMaker make) {
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  if (!stream_busy(p, c, make) && begin_wait(c, p, WAIT_RESPONSE, NULL, make)) {
    input(c);
  }
  stream_await(c);
}

void tz_http2_write(TzPromise *p, void *conn, void *bytes, TzMaker make) {
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  write_chunk(p, c, bytes, make);
  stream_await(c);
}

void tz_http2_request(void *conn, void *method_obj, uint64_t method_meta, void *target_obj,
                      uint64_t target_meta, void *headers, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  if (c->h2parent) {
    TzStr method = {method_obj, method_meta}, target = {target_obj, target_meta};
    tz_h2_request(c, method, target, (const TzArray *)headers, 0, 1);
  }
  conn_unlock(c);
}

int64_t tz_http2_respond(void *conn, int64_t status, void *headers, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  int64_t outcome = tz_h2_respond_once(c, (int32_t)status, (const TzArray *)headers);
  if (outcome == 0) {
    c->http.responded = 1;
  }
  conn_unlock(c);

  return outcome;
}
