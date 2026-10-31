// How long nghttp2 alone takes per request of the hello benchmark
// (bench/http2.ps1): a client session makes requests in batches of 8, a
// server session takes them, answers each with "Hello, World!" and the
// client takes the answers, all in memory, without sockets. Only the time of
// the server session is counted.
#include <nghttp2/nghttp2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef struct {
  uint8_t *data;
  size_t length, capacity;
} Buffer;

static void put(Buffer *b, const uint8_t *data, size_t length) {
  if (b->length + length > b->capacity) {
    b->capacity = (b->length + length) * 2;
    b->data = realloc(b->data, b->capacity);
  }
  memcpy(b->data + b->length, data, length);
  b->length += length;
}

static void drain(nghttp2_session *session, Buffer *out) {
  for (;;) {
    const uint8_t *data;
    nghttp2_ssize n = nghttp2_session_mem_send2(session, &data);
    if (n <= 0) {
      return;
    }
    put(out, data, (size_t)n);
  }
}

static const char kBody[] = "Hello, World!";

static nghttp2_ssize read_body(nghttp2_session *session, int32_t stream_id, uint8_t *buf,
                               size_t length, uint32_t *flags, nghttp2_data_source *source,
                               void *user_data) {
  (void)session;
  (void)stream_id;
  (void)length;
  (void)source;
  (void)user_data;
  memcpy(buf, kBody, sizeof kBody - 1);
  *flags |= NGHTTP2_DATA_FLAG_EOF;

  return sizeof kBody - 1;
}

static long long answered;

static int on_frame(nghttp2_session *session, const nghttp2_frame *frame, void *user_data) {
  (void)user_data;
  if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }
  nghttp2_nv nva[] = {
      {(uint8_t *)":status", (uint8_t *)"200", 7, 3, NGHTTP2_NV_FLAG_NONE},
      {(uint8_t *)"content-type", (uint8_t *)"text/plain;charset=UTF-8", 12, 24,
       NGHTTP2_NV_FLAG_NONE},
      {(uint8_t *)"content-length", (uint8_t *)"13", 14, 2, NGHTTP2_NV_FLAG_NONE},
      {(uint8_t *)"date", (uint8_t *)"Sat, 31 Oct 2026 07:12:17 GMT", 4, 29, NGHTTP2_NV_FLAG_NONE},
  };
  nghttp2_data_provider2 provider = {{0}, read_body};
  nghttp2_submit_response2(session, frame->hd.stream_id, nva, 4, &provider);
  ++answered;

  return 0;
}

static int on_header(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name,
                     size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags,
                     void *user_data) {
  (void)session;
  (void)frame;
  (void)name;
  (void)namelen;
  (void)value;
  (void)valuelen;
  (void)flags;
  (void)user_data;

  return 0;
}

int main(void) {
  nghttp2_session_callbacks *server_callbacks, *client_callbacks;
  nghttp2_session_callbacks_new(&server_callbacks);
  nghttp2_session_callbacks_set_on_frame_recv_callback(server_callbacks, on_frame);
  nghttp2_session_callbacks_set_on_header_callback(server_callbacks, on_header);
  nghttp2_session_callbacks_new(&client_callbacks);
  nghttp2_session *server, *client;
  nghttp2_session_server_new(&server, server_callbacks, NULL);
  nghttp2_session_client_new(&client, client_callbacks, NULL);
  nghttp2_settings_entry settings[] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100000}};
  nghttp2_submit_settings(server, NGHTTP2_FLAG_NONE, settings, 1);
  nghttp2_submit_settings(client, NGHTTP2_FLAG_NONE, NULL, 0);
  Buffer to_server = {0}, to_client = {0};
  nghttp2_nv request[] = {
      {(uint8_t *)":method", (uint8_t *)"GET", 7, 3, NGHTTP2_NV_FLAG_NONE},
      {(uint8_t *)":scheme", (uint8_t *)"http", 7, 4, NGHTTP2_NV_FLAG_NONE},
      {(uint8_t *)":authority", (uint8_t *)"127.0.0.1:19003", 10, 15, NGHTTP2_NV_FLAG_NONE},
      {(uint8_t *)":path", (uint8_t *)"/", 5, 1, NGHTTP2_NV_FLAG_NONE},
  };
  LARGE_INTEGER frequency, start, end;
  QueryPerformanceFrequency(&frequency);
  double server_seconds = 0;
  const int rounds = 100000;
  for (int round = 0; round < rounds; ++round) {
    for (int i = 0; i < 8; ++i) {
      nghttp2_submit_request2(client, NULL, request, 4, NULL, NULL);
    }
    to_server.length = 0;
    drain(client, &to_server);
    to_client.length = 0;
    QueryPerformanceCounter(&start);
    nghttp2_session_mem_recv2(server, to_server.data, to_server.length);
    drain(server, &to_client);
    QueryPerformanceCounter(&end);
    server_seconds += (double)(end.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
    nghttp2_session_mem_recv2(client, to_client.data, to_client.length);
  }
  printf("nghttp2 server: %lld requests, %.0f ns per request\n", answered,
         server_seconds * 1e9 / (double)answered);

  return 0;
}
