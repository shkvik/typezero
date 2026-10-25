// Buffer, and the text of bytes in an encoding.

#ifndef TZ_BUFFER_H
#define TZ_BUFFER_H

#include "runtime/core/string/string.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include <stddef.h>
#include <stdint.h>

// A new Buffer with a copy of `data`: readFileSync without an encoding.
TzBytes *tz_buffer_copy_of(const uint8_t *data, int64_t length);
// Buffer.from(text, encoding), and the text that writeFileSync writes with
// an encoding other than utf8 (the encodings numbered as in buffer.c).
TzBytes *tz_buffer_from_string(void *obj, uint64_t meta, int32_t encoding);
// The text of `length` bytes in `encoding`: readFileSync with an encoding
// other than utf8.
TzStr tz_buffer_decode(const uint8_t *data, int64_t length, int32_t encoding);

// buf.toString(encoding, start?, end?), `has` saying which positions were
// given (1 start, 2 end).
void tz_buffer_to_string(TzStr *out, const TzBytes *b, int32_t encoding, int64_t start, int64_t end,
                         int32_t has);

// The text of a TextDecoder's encoding, its `encoding` property.
const char *tz_text_decoder_name(const void *decoder);
// Whether a TextDecoder is fatal, and whether it ignores the BOM.
int32_t tz_text_decoder_flag(const void *decoder, int32_t which);

#endif
