#ifndef TZ_BUFFER_H
#define TZ_BUFFER_H

#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include <stddef.h>
#include <stdint.h>

TzBytes *tz_buffer_copy_of(const uint8_t *data, int64_t length);
TzStr tz_buffer_decode(const uint8_t *data, int64_t length, int32_t encoding);


const char *tz_text_decoder_name(const void *decoder);
int32_t tz_text_decoder_flag(const void *decoder, int32_t which);
void tz_text_decode(TzStr *out, void *decoder, const uint8_t *data, int64_t n, int32_t stream,
                    TzMaker make);

#endif
