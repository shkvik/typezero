#include "runtime/library/http/http2/http2.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/group/group.h"
#include "runtime/core/string/string.h"
#include <nghttp2/nghttp2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define H2_MAX_HEAD 16384
#define H2_STREAM_WINDOW (1024 * 1024)
#define H2_CONNECTION_WINDOW (16 * 1024 * 1024)

typedef struct H2 {
  nghttp2_session *session;
  NetConn *conn;
  NetConn *first;
  NetConn *ready_first, *ready_last;
  TzPromise *waiter;
  int32_t streams;
  uint8_t done;
  uint8_t sending;
  uint8_t client;
  uint8_t goaway;
  uint8_t started;
  uint8_t not_http2;
  uint8_t closed_here;
} H2;

typedef struct Field {
  uint8_t *data;
  size_t length;
} Field;

typedef struct H2Request {
  Field method, path, scheme, authority;
  TzArray *headers;
  TzArray *trailers;
  TzArray *send_trailers;
  size_t bytes;
  size_t unconsumed;
  int32_t status;
  int32_t code;
  uint8_t host;
  uint8_t head_done;
  uint8_t in_trailers;
  uint8_t sent_end;
  uint8_t abandon;
  uint8_t stopped;
  uint8_t reset_here;
} H2Request;

typedef struct Fields {
  nghttp2_nv *nva;
  char **names;
  uint8_t **values;
  uint32_t taken;
  size_t n;
} Fields;

static const char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

int tz_h2_preface(const NetConn *c) {
  size_t have = c->in_end - c->in_start;
  size_t want = sizeof kPreface - 1;
  size_t n = have < want ? have : want;
  if (memcmp(c->in + c->in_start, kPreface, n) != 0) {
    return 0;
  }

  return have >= want ? 1 : -1;
}

static H2 *session_of(const NetConn *s) { return (H2 *)s->h2parent->h2; }

static void link_stream(H2 *h, NetConn *s) {
  s->prev = NULL;
  s->next = h->first;
  if (h->first) {
    h->first->prev = s;
  }
  h->first = s;
  ++h->streams;
}

static void unlink_stream(H2 *h, NetConn *s) {
  if (s->prev) {
    s->prev->next = s->next;
  } else if (h->first == s) {
    h->first = s->next;
  }
  if (s->next) {
    s->next->prev = s->prev;
  }
  s->prev = s->next = NULL;
  --h->streams;
}

static void set_field(Field *f, const uint8_t *data, size_t length) {
  free(f->data);
  f->data = (uint8_t *)malloc(length ? length : 1);
  if (!f->data) {
    abort();
  }
  memcpy(f->data, data, length);
  f->length = length;
}

static void release_array(TzArray **list) {
  if (*list && tz_rc_dec(&(*list)->rc)) {
    tz_arr_free(*list);
  }
  *list = NULL;
}

static void free_request(NetConn *s) {
  H2Request *r = (H2Request *)s->h2;
  if (!r) {
    return;
  }
  s->h2 = NULL;
  free(r->method.data);
  free(r->path.data);
  free(r->scheme.data);
  free(r->authority.data);
  release_array(&r->headers);
  release_array(&r->trailers);
  release_array(&r->send_trailers);
  free(r);
}

static void wake(NetConn *s) {
  if (s->drain && (s->out_len - s->flight_len < NET_HIGH_WATER_MARK || s->closed || s->error)) {
    net_fulfill(&s->drain, NULL, 0);
  }
  if (s->http.waiting) {
    s->on_input(s);
  }
}

static void watch_reads(H2 *h);

static void detach(H2 *h, NetConn *s, int32_t error, int32_t code) {
  if (!s->h2open) {
    return;
  }
  s->h2open = 0;
  unlink_stream(h, s);
  if (s->h2id > 0) {
    nghttp2_session_set_stream_user_data(h->session, s->h2id, NULL);
  }
  H2Request *r = (H2Request *)s->h2;
  if (r) {
    if (r->code == H2_CODE_OPEN) {
      r->code = code;
    }
    if (r->unconsumed && !h->done) {
      nghttp2_session_consume_connection(h->session, r->unconsumed);
    }
    r->unconsumed = 0;
  }
  if (error && !s->eof) {
    s->error = error;
  }
  s->closed = 1;
  wake(s);
  tz_instance_release((TzInstance *)s);
  watch_reads(h);
}

static H2Request *new_request(NetConn *s) {
  H2Request *r = (H2Request *)calloc(1, sizeof(H2Request));
  if (!r) {
    abort();
  }
  r->headers = tz_arr_new(&tz_string_type, 0, 16);
  r->code = H2_CODE_OPEN;
  s->h2 = r;

  return r;
}

static TzStr text_of(const uint8_t *s, size_t n) {
  if (!n) {
    return tz_str_from_ascii("", 0);
  }
  int plain = 1;
  for (size_t i = 0; i < n && plain; ++i) {
    plain = s[i] < 0x80;
  }
  if (plain) {
    return tz_str_from_ascii((const char *)s, n);
  }
  TzBuf b;
  tz_buf_init(&b);
  tz_buf_bytes(&b, s, n);

  return tz_buf_take(&b);
}

static TzStr text_of_field(const Field *f) { return text_of(f->data, f->length); }

static int begin_headers(nghttp2_session *session, const nghttp2_frame *frame, void *user_data) {
  H2 *h = (H2 *)user_data;
  if (frame->hd.type != NGHTTP2_HEADERS) {
    return 0;
  }
  if (!h->client && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
    NetConn *s = net_stream_new(h->conn);
    new_request(s);
    s->h2id = frame->hd.stream_id;
    s->h2open = 1;
    link_stream(h, s);
    nghttp2_session_set_stream_user_data(session, s->h2id, s);
    return 0;
  }
  NetConn *s = (NetConn *)nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
  if (!s || !s->h2) {
    return 0;
  }
  H2Request *r = (H2Request *)s->h2;
  r->bytes = 0;
  if (r->head_done) {
    r->in_trailers = 1;
    if (!r->trailers) {
      r->trailers = tz_arr_new(&tz_string_type, 0, 8);
    }
    return 0;
  }
  r->status = 0;
  if (!r->headers) {
    r->headers = tz_arr_new(&tz_string_type, 0, 16);
  }

  return 0;
}

static void push_pair(TzArray *list, const uint8_t *name, size_t name_length, const uint8_t *value,
                      size_t value_length) {
  TzStr pair[2] = {tz_str_from_ascii((const char *)name, name_length),
                   text_of(value, value_length)};
  tz_arr_push_value(list, &pair[0]);
  tz_arr_push_value(list, &pair[1]);
}

static int on_header(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name,
                     size_t name_length, const uint8_t *value, size_t value_length, uint8_t flags,
                     void *user_data) {
  (void)flags;
  (void)user_data;
  NetConn *s = (NetConn *)nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
  if (!s || !s->h2) {
    return 0;
  }
  H2Request *r = (H2Request *)s->h2;
  r->bytes += name_length + value_length + 32;
  if (r->bytes > H2_MAX_HEAD) {
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  }
  if (r->in_trailers) {
    if (r->trailers && !(name_length && name[0] == ':')) {
      push_pair(r->trailers, name, name_length, value, value_length);
    }
    return 0;
  }
  if (name_length == 7 && memcmp(name, ":status", 7) == 0) {
    int32_t status = 0;
    for (size_t i = 0; i < value_length && i < 3; ++i) {
      status = value[i] >= '0' && value[i] <= '9' ? status * 10 + (value[i] - '0') : -1000;
    }
    r->status = value_length == 3 ? status : -1;
    return 0;
  }
  if (name_length && name[0] == ':') {
    Field *into = NULL;
    if (name_length == 7 && memcmp(name, ":method", 7) == 0) {
      into = &r->method;
    } else if (name_length == 5 && memcmp(name, ":path", 5) == 0) {
      into = &r->path;
    } else if (name_length == 7 && memcmp(name, ":scheme", 7) == 0) {
      into = &r->scheme;
    } else if (name_length == 10 && memcmp(name, ":authority", 10) == 0) {
      into = &r->authority;
    }
    if (into) {
      set_field(into, value, value_length);
    }
    return 0;
  }
  if (name_length == 4 && memcmp(name, "host", 4) == 0) {
    r->host = 1;
  }
  if (r->headers) {
    push_pair(r->headers, name, name_length, value, value_length);
  }

  return 0;
}

static void request_ready(H2 *h, NetConn *s, int ended) {
  H2Request *r = (H2Request *)s->h2;
  r->head_done = 1;
  if (!r->headers) {
    r->headers = tz_arr_new(&tz_string_type, 0, 2);
  }
  if (!r->host && r->authority.length) {
    TzStr pair[2] = {tz_str_from_ascii("host", 4), text_of_field(&r->authority)};
    tz_arr_push_value(r->headers, &pair[0]);
    tz_arr_push_value(r->headers, &pair[1]);
  }
  TzBuf target;
  tz_buf_init(&target);
  NetListener *l = h->conn->listener;
  const char *scheme = l && l->tls ? "https" : "http";
  if (r->scheme.length) {
    tz_buf_bytes(&target, r->scheme.data, r->scheme.length);
  } else {
    tz_buf_bytes(&target, (const uint8_t *)scheme, strlen(scheme));
  }
  tz_buf_bytes(&target, (const uint8_t *)"://", 3);
  if (r->authority.length) {
    tz_buf_bytes(&target, r->authority.data, r->authority.length);
  } else {
    char text[300];
    const char *host = l ? l->host : "localhost";
    int bracket = strchr(host, ':') != NULL;
    int n = snprintf(text, sizeof text, "%s%s%s:%d", bracket ? "[" : "", host, bracket ? "]" : "",
                     l ? (int)l->port : 80);
    tz_buf_bytes(&target, (const uint8_t *)text, (size_t)n);
  }
  tz_buf_bytes(&target, r->path.data, r->path.length);
  int head = r->method.length == 4 && memcmp(r->method.data, "HEAD", 4) == 0;
  TzArray *headers = r->headers;
  r->headers = NULL;
  tz_http_stream_request(s, text_of_field(&r->method), tz_buf_take(&target), headers, head, ended);
  s->h2fresh = 1;
  if (!l || l->closed) {
    nghttp2_submit_rst_stream(h->session, NGHTTP2_FLAG_NONE, s->h2id, NGHTTP2_REFUSED_STREAM);
    return;
  }
  tz_rc_inc(&s->rc);
  if (h->waiter) {
    net_fulfill(&h->waiter, &s, sizeof s);
    return;
  }
  s->ready_next = NULL;
  if (h->ready_last) {
    h->ready_last->ready_next = s;
  } else {
    h->ready_first = s;
  }
  h->ready_last = s;
}

static NetConn *take_ready(H2 *h) {
  NetConn *s = h->ready_first;
  if (s) {
    h->ready_first = s->ready_next;
    if (!h->ready_first) {
      h->ready_last = NULL;
    }
    s->ready_next = NULL;
  }

  return s;
}

void tz_http_stream(TzPromise *p, void *conn, TzMaker make) {
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  H2 *h = (H2 *)c->h2;
  tz_net_make = make;
  tz_ref_retain(p);
  NetConn *s = h ? take_ready(h) : NULL;
  if (s || !h || h->done) {
    net_fulfill(&p, &s, sizeof s);
  } else {
    if (h->waiter) {
      TzPromise *old = h->waiter;
      h->waiter = NULL;
      tz_obj_release((TzObject *)old);
    }
    h->waiter = p;
  }
#if TZ_PARALLEL
  tz_thread.await_lock = conn_lock_of(c);
#else
  conn_unlock(c);
#endif
}

static void response_ready(NetConn *s, int ended) {
  H2Request *r = (H2Request *)s->h2;
  if (r->status >= 100 && r->status < 200) {
    release_array(&r->headers);
    r->status = 0;
    return;
  }
  r->head_done = 1;
  TzArray *headers = r->headers;
  r->headers = NULL;
  int32_t status = r->status;
  tz_http_stream_response(s, status < 200 ? 502 : status, headers, ended);
  s->h2fresh = 1;
  wake(s);
}

static int on_frame(nghttp2_session *session, const nghttp2_frame *frame, void *user_data) {
  H2 *h = (H2 *)user_data;
  if (frame->hd.type == NGHTTP2_GOAWAY) {
    h->goaway = 1;
    return 0;
  }
  NetConn *s = (NetConn *)nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
  if (!s) {
    return 0;
  }
  int ended = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0;
  if (frame->hd.type == NGHTTP2_HEADERS && s->h2 && (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS)) {
    H2Request *r = (H2Request *)s->h2;
    if (r->in_trailers) {
      r->in_trailers = 0;
    } else if (!r->head_done) {
      if (h->client) {
        response_ready(s, ended);
      } else {
        request_ready(h, s, ended);
      }
      return 0;
    }
  }
  if (ended && (frame->hd.type == NGHTTP2_DATA || frame->hd.type == NGHTTP2_HEADERS)) {
    s->eof = 1;
    wake(s);
  }

  return 0;
}

static void stop_reading(nghttp2_session *session, NetConn *s) {
  H2Request *r = (H2Request *)s->h2;
  if (!r || r->stopped || s->eof || !s->h2open || s->h2id <= 0) {
    return;
  }
  r->stopped = 1;
  nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, s->h2id, NGHTTP2_NO_ERROR);
}

static int on_frame_send(nghttp2_session *session, const nghttp2_frame *frame, void *user_data) {
  (void)user_data;
  if ((frame->hd.type != NGHTTP2_DATA && frame->hd.type != NGHTTP2_HEADERS) ||
      !(frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
    return 0;
  }
  NetConn *s = (NetConn *)nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
  if (!s || !s->h2) {
    return 0;
  }
  H2Request *r = (H2Request *)s->h2;
  r->sent_end = 1;
  if (r->abandon) {
    stop_reading(session, s);
  }

  return 0;
}

static int on_data(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data,
                   size_t length, void *user_data) {
  (void)flags;
  H2 *h = (H2 *)user_data;
  NetConn *s = (NetConn *)nghttp2_session_get_stream_user_data(session, stream_id);
  if (!length) {
    return 0;
  }
  if (!s) {
    nghttp2_session_consume_connection(h->session, length);
    return 0;
  }
  if (s->h2) {
    ((H2Request *)s->h2)->unconsumed += length;
  }
  if (s->in_start == s->in_end) {
    s->in_start = s->in_end = 0;
  }
  if (s->in_cap - s->in_end < length) {
    size_t have = s->in_end - s->in_start;
    size_t cap = (size_t)s->in_cap * 2 + length + 4096;
    uint8_t *in = (uint8_t *)tz_alloc(cap);
    if (have) {
      memcpy(in, s->in + s->in_start, have);
    }
    tz_free(s->in);
    s->in = in;
    s->in_start = 0;
    s->in_end = (uint32_t)have;
    s->in_cap = (uint32_t)cap;
  }
  memcpy(s->in + s->in_end, data, length);
  s->in_end += (uint32_t)length;
  wake(s);

  return 0;
}

static int on_close(nghttp2_session *session, int32_t stream_id, uint32_t error_code,
                    void *user_data) {
  H2 *h = (H2 *)user_data;
  NetConn *s = (NetConn *)nghttp2_session_get_stream_user_data(session, stream_id);
  if (!s) {
    return 0;
  }
  const H2Request *r = (const H2Request *)s->h2;
  int32_t error = 0;
  if (error_code == NGHTTP2_REFUSED_STREAM && h->client && !(r && r->head_done)) {
    error = H2_REFUSED;
  } else if (error_code != NGHTTP2_NO_ERROR || !s->eof) {
    error = h->client ? H2_RESET : -1;
  }
  detach(h, s, error, (int32_t)error_code);

  return 0;
}

static int lower_name(TzStr name, char *out, size_t size, size_t *length) {
  uint32_t n;
  int two;
  const void *units = tz_str_units(&name, &n, &two);
  if (n == 0 || n >= size) {
    return 0;
  }
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t c = two ? ((const uint16_t *)units)[i] : ((const uint8_t *)units)[i];
    if (c <= 0x20 || c >= 0x7F || c == ':') {
      return 0;
    }
    out[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
  }
  out[n] = 0;
  *length = n;

  return 1;
}

static int value_bytes(TzStr value, uint8_t **out, size_t *length) {
  uint32_t n;
  int two;
  const void *units = tz_str_units(&value, &n, &two);
  uint8_t *bytes = (uint8_t *)malloc(n ? n : 1);
  if (!bytes) {
    abort();
  }
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t c = two ? ((const uint16_t *)units)[i] : ((const uint8_t *)units)[i];
    if (c == '\r' || c == '\n' || c == 0 || c > 0xFF) {
      free(bytes);
      return 0;
    }
    bytes[i] = (uint8_t)c;
  }
  *out = bytes;
  *length = n;

  return 1;
}

static int hop_by_hop(const char *name) {
  return strcmp(name, "connection") == 0 || strcmp(name, "keep-alive") == 0 ||
         strcmp(name, "transfer-encoding") == 0 || strcmp(name, "upgrade") == 0 ||
         strcmp(name, "proxy-connection") == 0;
}

static void fields_init(Fields *f, uint32_t count, size_t extra) {
  f->nva = (nghttp2_nv *)calloc((size_t)count + extra, sizeof(nghttp2_nv));
  f->names = (char **)calloc((size_t)count + 1, sizeof(char *));
  f->values = (uint8_t **)calloc((size_t)count + 1, sizeof(uint8_t *));
  if (!f->nva || !f->names || !f->values) {
    abort();
  }
  f->taken = 0;
  f->n = 0;
}

static void fields_add(Fields *f, const char *name, size_t name_length, uint8_t *value,
                       size_t value_length) {
  char *copy = (char *)malloc(name_length + 1);
  if (!copy) {
    abort();
  }
  memcpy(copy, name, name_length + 1);
  f->names[f->taken] = copy;
  f->values[f->taken] = value;
  ++f->taken;
  f->nva[f->n++] =
      (nghttp2_nv){(uint8_t *)copy, value, name_length, value_length, NGHTTP2_NV_FLAG_NONE};
}

static void fields_free(Fields *f) {
  for (uint32_t i = 0; i < f->taken; ++i) {
    free(f->names[i]);
    free(f->values[i]);
  }
  free(f->names);
  free(f->values);
  free(f->nva);
}

static int trailer_allowed(const char *name) {
  return !hop_by_hop(name) && strcmp(name, "content-length") != 0 && strcmp(name, "host") != 0 &&
         strcmp(name, "te") != 0;
}

static void submit_trailers(nghttp2_session *session, int32_t stream_id, const TzArray *list) {
  uint32_t count = list->length / 2;
  Fields f;
  fields_init(&f, count, 1);
  const TzStr *items = (const TzStr *)list->data;
  for (uint32_t i = 0; i < count; ++i) {
    char name[64];
    size_t name_length, value_length;
    uint8_t *value;
    if (!lower_name(items[2 * i], name, sizeof name, &name_length) || !trailer_allowed(name) ||
        !value_bytes(items[2 * i + 1], &value, &value_length)) {
      continue;
    }
    fields_add(&f, name, name_length, value, value_length);
  }
  nghttp2_submit_trailer(session, stream_id, f.nva, f.n);
  fields_free(&f);
}

static nghttp2_ssize read_body(nghttp2_session *session, int32_t stream_id, uint8_t *buf,
                               size_t length, uint32_t *data_flags, nghttp2_data_source *source,
                               void *user_data) {
  (void)user_data;
  NetConn *s = (NetConn *)source->ptr;
  size_t have = s->out_len - s->flight_len;
  size_t n = have < length ? have : length;
  if (n) {
    memcpy(buf, s->out + s->flight_len, n);
    s->flight_len += (uint32_t)n;
  }
  if (s->flight_len == s->out_len) {
    s->flight_len = s->out_len = 0;
    if (s->h2ended) {
      *data_flags |= NGHTTP2_DATA_FLAG_EOF;
      H2Request *r = (H2Request *)s->h2;
      if (r && r->send_trailers && r->send_trailers->length >= 2) {
        *data_flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
        submit_trailers(session, stream_id, r->send_trailers);
        release_array(&r->send_trailers);
      }
    } else if (!n) {
      return NGHTTP2_ERR_DEFERRED;
    }
  }
  if (s->drain && s->out_len - s->flight_len < NET_HIGH_WATER_MARK) {
    net_fulfill(&s->drain, NULL, 0);
  }

  return (nghttp2_ssize)n;
}

static int32_t lost_code(const H2 *h) {
  if (h->not_http2) {
    return H2_CODE_NOT_HTTP2;
  }

  return h->closed_here ? H2_CODE_CLOSED_HERE : H2_CODE_LOST;
}

static void close_all(H2 *h) {
  while (h->first) {
    detach(h, h->first, h->client ? H2_RESET : -1, lost_code(h));
  }
}

static void watch_reads(H2 *h) {
  NetConn *c = h->conn;
  if (!h->client || !c->reading || c->pooled) {
    return;
  }
  if (h->streams > 0) {
    tz_work_ref(&c->rop.work);
  } else {
    tz_work_unref(&c->rop.work);
  }
}

static void finish(NetConn *c) {
  H2 *h = (H2 *)c->h2;
  if (h->done) {
    return;
  }
  h->done = 1;
  close_all(h);
  net_close(c);
  if (h->waiter) {
    NetConn *none = NULL;
    net_fulfill(&h->waiter, &none, sizeof none);
  }
}

static void send_frames(NetConn *c) {
  H2 *h = (H2 *)c->h2;
  if (h->sending || h->done) {
    return;
  }
  h->sending = 1;
  for (;;) {
    const uint8_t *data;
    nghttp2_ssize n = nghttp2_session_mem_send2(h->session, &data);
    if (n < 0) {
      h->sending = 0;
      net_abort(c);
      finish(c);
      return;
    }
    if (n == 0) {
      break;
    }
    net_write(c, data, (size_t)n);
  }
  h->sending = 0;
  net_flush(c);
  if (!nghttp2_session_want_read(h->session) && !nghttp2_session_want_write(h->session)) {
    finish(c);
  }
}

static void session_input(NetConn *c) {
  H2 *h = (H2 *)c->h2;
  if (h->done) {
    return;
  }
  if (c->in_end > c->in_start) {
    if (h->client && !h->started) {
      size_t have = c->in_end - c->in_start;
      if (have < 5) {
        if (!c->eof && !c->error && !c->closed) {
          net_read(c);
          return;
        }
      } else {
        h->started = 1;
        if (memcmp(c->in + c->in_start, "HTTP/", 5) == 0) {
          h->not_http2 = 1;
          c->in_start = c->in_end = 0;
          net_abort(c);
          finish(c);
          return;
        }
      }
    }
    nghttp2_ssize n =
        nghttp2_session_mem_recv2(h->session, c->in + c->in_start, c->in_end - c->in_start);
    c->in_start = c->in_end = 0;
    if (n < 0) {
      send_frames(c);
      net_abort(c);
      finish(c);
      return;
    }
  }
  send_frames(c);
  if (h->done) {
    return;
  }
  if (c->eof || c->error || c->closed) {
    finish(c);
    return;
  }
  net_read(c);
  watch_reads(h);
}

static nghttp2_session_callbacks *callbacks_new(void) {
  nghttp2_session_callbacks *callbacks;
  nghttp2_session_callbacks_new(&callbacks);
  nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, begin_headers);
  nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame);
  nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, on_frame_send);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data);
  nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_close);

  return callbacks;
}

void tz_h2_start(NetConn *c) {
  H2 *h = (H2 *)calloc(1, sizeof(H2));
  if (!h) {
    abort();
  }
  h->conn = c;
  nghttp2_session_callbacks *callbacks = callbacks_new();
  nghttp2_option *option;
  nghttp2_option_new(&option);
  nghttp2_option_set_max_send_header_block_length(option, H2_MAX_HEAD * 4);
  nghttp2_option_set_no_auto_window_update(option, 1);
  nghttp2_session_server_new3(&h->session, callbacks, h, option, NULL);
  nghttp2_option_del(option);
  nghttp2_session_callbacks_del(callbacks);
  NetListener *l = c->listener;
  uint32_t streams = l && l->h2_streams ? l->h2_streams : 100;
  nghttp2_settings_entry settings[] = {
      {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, streams},
      {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, H2_STREAM_WINDOW},
      {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, H2_MAX_HEAD},
      {NGHTTP2_SETTINGS_ENABLE_PUSH, 0},
  };
  nghttp2_submit_settings(h->session, NGHTTP2_FLAG_NONE, settings,
                          sizeof settings / sizeof settings[0]);
  nghttp2_session_set_local_window_size(h->session, NGHTTP2_FLAG_NONE, 0, H2_CONNECTION_WINDOW);
  c->h2 = h;
  c->busy = 0;
  c->on_input = session_input;
  session_input(c);
}

void tz_h2_respond(NetConn *s, int32_t status, const TzArray *headers, int no_body, int stream) {
  H2 *h = session_of(s);
  if (!s->h2open || s->h2submitted) {
    s->out_len = s->flight_len = 0;
    return;
  }
  uint32_t count = headers ? headers->length / 2 : 0;
  Fields f;
  fields_init(&f, count, 3);
  char status_text[16];
  int status_length = snprintf(status_text, sizeof status_text, "%d", (int)status);
  f.nva[f.n++] = (nghttp2_nv){(uint8_t *)":status", (uint8_t *)status_text, 7,
                              (size_t)status_length, NGHTTP2_NV_FLAG_NO_COPY_NAME};
  int date = 0, length_given = 0;
  const TzStr *items = headers ? (const TzStr *)headers->data : NULL;
  for (uint32_t i = 0; i < count; ++i) {
    char name[64];
    size_t name_length, value_length;
    uint8_t *value;
    if (!lower_name(items[2 * i], name, sizeof name, &name_length) || hop_by_hop(name) ||
        !value_bytes(items[2 * i + 1], &value, &value_length)) {
      continue;
    }
    if (strcmp(name, "content-length") == 0) {
      if (!no_body && !stream) {
        free(value);
        continue;
      }
      length_given = 1;
    }
    if (strcmp(name, "date") == 0) {
      date = 1;
    }
    fields_add(&f, name, name_length, value, value_length);
  }
  char length_text[24];
  if (!date) {
    const char *now = tz_http_date_text();
    f.nva[f.n++] = (nghttp2_nv){(uint8_t *)"date", (uint8_t *)now, 4, strlen(now),
                                NGHTTP2_NV_FLAG_NO_COPY_NAME};
  }
  if (!no_body && !stream && !length_given) {
    int written = snprintf(length_text, sizeof length_text, "%u", (unsigned)s->out_len);
    f.nva[f.n++] = (nghttp2_nv){(uint8_t *)"content-length", (uint8_t *)length_text, 14,
                                (size_t)written, NGHTTP2_NV_FLAG_NO_COPY_NAME};
  }
  s->h2submitted = 1;
  if (no_body) {
    s->out_len = s->flight_len = 0;
    s->h2ended = 1;
    nghttp2_submit_response2(h->session, s->h2id, f.nva, f.n, NULL);
  } else {
    s->h2ended = !stream;
    nghttp2_data_provider2 provider;
    provider.source.ptr = s;
    provider.read_callback = read_body;
    nghttp2_submit_response2(h->session, s->h2id, f.nva, f.n, &provider);
  }
  fields_free(&f);
  send_frames(s->h2parent);
}

void tz_http_stream_done(void *conn, TzMaker make) {
  (void)make;
  NetConn *s = (NetConn *)conn;
  conn_lock(s);
  net_close(s);
  conn_unlock(s);
}

void tz_h2_flush(NetConn *s) {
  if (!s->h2open || !s->h2submitted) {
    return;
  }
  H2 *h = session_of(s);
  nghttp2_session_resume_data(h->session, s->h2id);
  send_frames(s->h2parent);
}

void tz_h2_end(NetConn *s) {
  if (!s->h2open) {
    return;
  }
  if (!s->h2submitted) {
    tz_h2_reset(s);
    return;
  }
  H2Request *r = (H2Request *)s->h2;
  if (s->server && s->closing && r && !s->eof) {
    r->abandon = 1;
    if (r->sent_end) {
      stop_reading(session_of(s)->session, s);
    }
  }
  s->h2ended = 1;
  tz_h2_flush(s);
}

static void reset_stream(NetConn *s, uint32_t code) {
  s->out_len = s->flight_len = 0;
  if (!s->h2open) {
    s->closed = 1;
    return;
  }
  H2Request *r = (H2Request *)s->h2;
  if (r) {
    r->reset_here = 1;
  }
  H2 *h = session_of(s);
  if (s->h2id <= 0) {
    detach(h, s, s->server ? -1 : H2_RESET, (int32_t)code);
    return;
  }
  nghttp2_submit_rst_stream(h->session, NGHTTP2_FLAG_NONE, s->h2id, code);
  send_frames(s->h2parent);
}

void tz_h2_reset(NetConn *s) { reset_stream(s, NGHTTP2_INTERNAL_ERROR); }

void tz_h2_dropped(NetConn *s) { free_request(s); }

void tz_h2_shutdown(NetConn *c) {
  H2 *h = (H2 *)c->h2;
  if (!h || h->done) {
    return;
  }
  nghttp2_submit_shutdown_notice(h->session);
  nghttp2_submit_goaway(h->session, NGHTTP2_FLAG_NONE,
                        nghttp2_session_get_last_proc_stream_id(h->session), NGHTTP2_NO_ERROR, NULL,
                        0);
  send_frames(c);
}

int tz_h2_idle(const NetConn *c) {
  const H2 *h = (const H2 *)c->h2;

  return !h || h->streams == 0;
}

void tz_h2_release(NetConn *c) {
  H2 *h = (H2 *)c->h2;
  if (!h) {
    return;
  }
  c->h2 = NULL;
  close_all(h);
  for (NetConn *s = take_ready(h); s; s = take_ready(h)) {
    tz_instance_release((TzInstance *)s);
  }
  if (h->waiter) {
    TzPromise *p = h->waiter;
    h->waiter = NULL;
    tz_obj_release((TzObject *)p);
  }
  nghttp2_session_del(h->session);
  free(h);
}

void tz_h2_consumed(NetConn *s, size_t length) {
  H2Request *r = (H2Request *)s->h2;
  if (!r || !length) {
    return;
  }
  size_t n = length < r->unconsumed ? length : r->unconsumed;
  r->unconsumed -= n;
  if (n && s->h2open && s->h2id > 0) {
    H2 *h = session_of(s);
    if (!h->done) {
      nghttp2_session_consume(h->session, s->h2id, n);
    }
  }
}

void tz_h2_send_pending(NetConn *s) {
  NetConn *c = s->h2parent;
  H2 *h = c ? (H2 *)c->h2 : NULL;
  if (h && !h->done && nghttp2_session_want_write(h->session)) {
    send_frames(c);
  }
}

void tz_http_offer_h2(void *conn, TzMaker make) {
  (void)make;
  ((NetConn *)conn)->h2offer = 1;
}

int32_t tz_http_is_h2(void *conn, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;

  return c->tls && c->tls_ops->h2 && c->tls_ops->h2(c);
}

void tz_http_start_client(void *conn, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  H2 *h = (H2 *)calloc(1, sizeof(H2));
  if (!h) {
    abort();
  }
  h->conn = c;
  h->client = 1;
  nghttp2_session_callbacks *callbacks = callbacks_new();
  nghttp2_option *option;
  nghttp2_option_new(&option);
  nghttp2_option_set_no_auto_window_update(option, 1);
  nghttp2_session_client_new3(&h->session, callbacks, h, option, NULL);
  nghttp2_option_del(option);
  nghttp2_session_callbacks_del(callbacks);
  nghttp2_settings_entry settings[] = {
      {NGHTTP2_SETTINGS_ENABLE_PUSH, 0},
      {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, H2_STREAM_WINDOW},
      {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, H2_MAX_HEAD * 4},
  };
  nghttp2_submit_settings(h->session, NGHTTP2_FLAG_NONE, settings,
                          sizeof settings / sizeof settings[0]);
  nghttp2_session_set_local_window_size(h->session, NGHTTP2_FLAG_NONE, 0, H2_CONNECTION_WINDOW);
  c->h2 = h;
  c->on_input = session_input;
  session_input(c);
  conn_unlock(c);
}

void *tz_http_open_stream(void *conn, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  H2 *h = (H2 *)c->h2;
  if (!h || h->done || h->goaway || c->closed || c->error) {
    conn_unlock(c);
    return NULL;
  }
  NetConn *s = net_stream_new(c);
  s->server = 0;
  s->h2open = 1;
  new_request(s);
  link_stream(h, s);
  tz_rc_inc(&s->rc);
  watch_reads(h);
  conn_unlock(c);

  return s;
}

int64_t tz_http_streams_left(void *conn, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  H2 *h = (H2 *)c->h2;
  int64_t left = -1;
  if (h && !h->done && !h->goaway && !c->closed && !c->error) {
    uint32_t most =
        nghttp2_session_get_remote_settings(h->session, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
    left = (int64_t)most - h->streams;
  }
  conn_unlock(c);

  return left;
}

static int skipped_request_header(const char *name, const uint8_t *value, size_t value_length) {
  if (strcmp(name, "te") == 0) {
    return !(value_length == 8 && memcmp(value, "trailers", 8) == 0);
  }

  return hop_by_hop(name) || strcmp(name, "host") == 0;
}

void tz_h2_request(NetConn *s, TzStr method, TzStr target, const TzArray *headers, int no_body,
                   int stream) {
  H2 *h = session_of(s);
  if (!s->h2open || s->h2submitted) {
    s->out_len = s->flight_len = 0;
    return;
  }
  uint32_t count = headers ? headers->length / 2 : 0;
  Fields f;
  fields_init(&f, count, 4);
  uint8_t *method_bytes, *path_bytes, *authority = NULL;
  size_t method_length, path_length, authority_length = 0;
  if (!value_bytes(method, &method_bytes, &method_length)) {
    method_bytes = (uint8_t *)malloc(3);
    if (!method_bytes) {
      abort();
    }
    memcpy(method_bytes, "GET", 3);
    method_length = 3;
  }
  if (!value_bytes(target, &path_bytes, &path_length)) {
    path_bytes = (uint8_t *)malloc(1);
    if (!path_bytes) {
      abort();
    }
    path_bytes[0] = '/';
    path_length = 1;
  }
  const char *scheme = s->h2parent->tls ? "https" : "http";
  f.nva[f.n++] = (nghttp2_nv){(uint8_t *)":method", method_bytes, 7, method_length,
                              NGHTTP2_NV_FLAG_NO_COPY_NAME};
  f.nva[f.n++] = (nghttp2_nv){(uint8_t *)":scheme", (uint8_t *)scheme, 7, strlen(scheme),
                              NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE};
  size_t authority_at = f.n++;
  f.nva[f.n++] =
      (nghttp2_nv){(uint8_t *)":path", path_bytes, 5, path_length, NGHTTP2_NV_FLAG_NO_COPY_NAME};
  const TzStr *items = headers ? (const TzStr *)headers->data : NULL;
  for (uint32_t i = 0; i < count; ++i) {
    char name[64];
    size_t name_length, value_length;
    uint8_t *value;
    if (!lower_name(items[2 * i], name, sizeof name, &name_length) ||
        !value_bytes(items[2 * i + 1], &value, &value_length)) {
      continue;
    }
    if (strcmp(name, "host") == 0) {
      if (!authority) {
        authority = value;
        authority_length = value_length;
      } else {
        free(value);
      }
      continue;
    }
    if (skipped_request_header(name, value, value_length) ||
        (strcmp(name, "content-length") == 0 && stream)) {
      free(value);
      continue;
    }
    fields_add(&f, name, name_length, value, value_length);
  }
  f.nva[authority_at] = (nghttp2_nv){
      (uint8_t *)":authority", authority ? authority : (uint8_t *)s->h2parent->peer, 10,
      authority ? authority_length : strlen(s->h2parent->peer), NGHTTP2_NV_FLAG_NO_COPY_NAME};
  s->h2submitted = 1;
  int32_t id;
  if (no_body) {
    s->out_len = s->flight_len = 0;
    s->h2ended = 1;
    id = nghttp2_submit_request2(h->session, NULL, f.nva, f.n, NULL, s);
  } else {
    s->h2ended = !stream;
    nghttp2_data_provider2 provider;
    provider.source.ptr = s;
    provider.read_callback = read_body;
    id = nghttp2_submit_request2(h->session, NULL, f.nva, f.n, &provider, s);
  }
  fields_free(&f);
  free(method_bytes);
  free(path_bytes);
  free(authority);
  if (id < 0) {
    detach(h, s, H2_REFUSED, NGHTTP2_REFUSED_STREAM);
    return;
  }
  s->h2id = id;
  send_frames(s->h2parent);
}

void tz_h2_cancel(NetConn *s) { reset_stream(s, NGHTTP2_CANCEL); }

int64_t tz_http2_end(void *conn, void *trailers, TzMaker make) {
  (void)make;
  NetConn *s = (NetConn *)conn;
  conn_lock(s);
  int64_t outcome = 2;
  H2Request *r = (H2Request *)s->h2;
  if (!s->h2parent || !r) {
    outcome = 2;
  } else if (s->h2ended) {
    outcome = 1;
  } else if (!s->h2open || !s->h2submitted) {
    s->h2ended = 1;
    outcome = 2;
  } else {
    TzArray *list = (TzArray *)trailers;
    if (list && list->length >= 2) {
      tz_rc_inc(&list->rc);
      release_array(&r->send_trailers);
      r->send_trailers = list;
    }
    s->h2ended = 1;
    tz_h2_flush(s);
    outcome = 0;
  }
  conn_unlock(s);

  return outcome;
}

int64_t tz_h2_respond_once(NetConn *s, int32_t status, const TzArray *headers) {
  if (!s->h2parent || !s->h2) {
    return 2;
  }
  if (s->h2submitted) {
    return 1;
  }
  if (!s->h2open) {
    return 2;
  }
  tz_h2_respond(s, status, headers, 0, 1);

  return 0;
}

int tz_h2_head_done(const NetConn *s) {
  const H2Request *r = s->h2parent ? (const H2Request *)s->h2 : NULL;

  return r && r->head_done;
}

int64_t tz_http2_flags(void *conn, TzMaker make) {
  (void)make;
  NetConn *s = (NetConn *)conn;
  conn_lock(s);
  const H2Request *r = s->h2parent ? (const H2Request *)s->h2 : NULL;
  int64_t flags = (s->h2submitted ? 1 : 0) | (s->h2ended ? 2 : 0) | (s->http.body_done ? 4 : 0) |
                  (r && r->reset_here ? 8 : 0);
  conn_unlock(s);

  return flags;
}

static TzArray *copy_strings(const TzArray *list) {
  uint32_t n = list ? list->length : 0;
  TzArray *copy = tz_arr_new(&tz_string_type, 0, n ? n : 1);
  const TzStr *items = list ? (const TzStr *)list->data : NULL;
  for (uint32_t i = 0; i < n; ++i) {
    tz_str_retain_value(items[i]);
    tz_arr_push_value(copy, &items[i]);
  }

  return copy;
}

void *tz_http2_headers(void *conn, TzMaker make) {
  (void)make;
  NetConn *s = (NetConn *)conn;
  conn_lock(s);
  TzArray *copy = copy_strings(s->http.headers);
  conn_unlock(s);

  return copy;
}

void tz_http2_reset(void *conn, int64_t code, TzMaker make) {
  (void)make;
  NetConn *s = (NetConn *)conn;
  conn_lock(s);
  if (s->h2parent) {
    reset_stream(s, (uint32_t)code);
  }
  conn_unlock(s);
}

int64_t tz_http2_error_code(void *conn, TzMaker make) {
  (void)make;
  NetConn *s = (NetConn *)conn;
  conn_lock(s);
  const H2Request *r = s->h2parent ? (const H2Request *)s->h2 : NULL;
  int64_t code = r ? r->code : H2_CODE_LOST;
  conn_unlock(s);

  return code;
}

void tz_http2_field(TzStr *out, void *conn, int64_t which, TzMaker make) {
  (void)make;
  NetConn *s = (NetConn *)conn;
  conn_lock(s);
  const H2Request *r = s->h2parent ? (const H2Request *)s->h2 : NULL;
  if (!r) {
    *out = tz_str_from_ascii("", 0);
  } else {
    const Field *f = which == 0   ? &r->method
                     : which == 1 ? &r->scheme
                     : which == 2 ? &r->authority
                                  : &r->path;
    *out = text_of_field(f);
  }
  conn_unlock(s);
}

void *tz_http2_trailers(void *conn, TzMaker make) {
  (void)make;
  NetConn *s = (NetConn *)conn;
  conn_lock(s);
  const H2Request *r = s->h2parent ? (const H2Request *)s->h2 : NULL;
  TzArray *copy = copy_strings(r ? r->trailers : NULL);
  conn_unlock(s);

  return copy;
}

int64_t tz_http2_connection_state(void *conn, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  const H2 *h = (const H2 *)c->h2;
  int64_t state = 0;
  if (!h) {
    state = 2;
  } else if (h->not_http2) {
    state = 3;
  } else if (h->closed_here) {
    state = 4;
  } else if (h->done || c->closed || c->error) {
    state = 2;
  } else if (h->goaway) {
    state = 1;
  }
  conn_unlock(c);

  return state;
}

void tz_http2_close(void *conn, TzMaker make) {
  (void)make;
  NetConn *c = (NetConn *)conn;
  conn_lock(c);
  H2 *h = (H2 *)c->h2;
  if (h && !h->done) {
    h->closed_here = 1;
    nghttp2_session_terminate_session(h->session, NGHTTP2_NO_ERROR);
    send_frames(c);
    if (!h->done) {
      finish(c);
    }
  } else {
    net_close(c);
  }
  conn_unlock(c);
}
