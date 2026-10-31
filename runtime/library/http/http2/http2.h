#ifndef TZ_HTTP2_H
#define TZ_HTTP2_H

#include "runtime/library/net/net/net.h"

#define H2_REFUSED (-10)
#define H2_RESET (-11)

#define H2_CODE_OPEN (-2)
#define H2_CODE_LOST (-1)
#define H2_CODE_NOT_HTTP2 (-3)
#define H2_CODE_CLOSED_HERE (-4)

int tz_h2_preface(const NetConn *c);
void tz_h2_request(NetConn *s, TzStr method, TzStr target, const TzArray *headers, int no_body,
                   int stream);
void tz_h2_cancel(NetConn *s);
void tz_h2_start(NetConn *c);
void tz_h2_respond(NetConn *s, int32_t status, const TzArray *headers, int no_body, int stream);
void tz_h2_flush(NetConn *s);
void tz_h2_end(NetConn *s);
void tz_h2_reset(NetConn *s);
void tz_h2_dropped(NetConn *s);
void tz_h2_shutdown(NetConn *c);
int tz_h2_idle(const NetConn *c);
void tz_h2_release(NetConn *c);
void tz_h2_consumed(NetConn *s, size_t length);
void tz_h2_send_pending(NetConn *s);
void tz_h2_send_listed(NetConn *c);
int tz_h2_head_done(const NetConn *s);
int64_t tz_h2_respond_once(NetConn *s, int32_t status, const TzArray *headers);

void tz_http_stream(TzPromise *p, void *conn, TzMaker make);
void tz_http_stream_done(void *conn, TzMaker make);
void tz_http_stream_input(NetConn *c);
void tz_http_stream_request(NetConn *s, TzStr method, TzStr target, TzArray *headers, int head,
                            int ended);
const char *tz_http_date_text(void);
void tz_http_stream_response(NetConn *s, int32_t status, TzArray *headers, int ended);

#endif
