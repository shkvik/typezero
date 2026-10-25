// JSON.stringify of any value.

#ifndef TZ_JSON_STRINGIFY_H
#define TZ_JSON_STRINGIFY_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

// QuoteJSONString: the escapes of JSON, \u with lowercase hex for the other
// control characters and for lone surrogates.
void tz_json_quote(TzBuf *out, TzStr s);

#endif
