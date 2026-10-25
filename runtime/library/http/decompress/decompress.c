// What fetch gets compressed (docs/net.md, section 2), decoded as it comes:
// gzip and deflate by zlib's inflate, br by brotli's decoder, both in
// third_party. Only fetch calls this file, so only a program that fetches
// has it: the linker drops what nothing calls, the dictionary of brotli
// (122 KB) too.

#include "runtime/library/net/net/net.h"

#include <string.h>

#include "zlib.h"
#include <brotli/decode.h>

enum { CODING_GZIP = 1, CODING_DEFLATE = 2, CODING_BR = 3 };

typedef struct Decoder {
  int64_t rc;
  const TzLayout *layout;
  int32_t kind;
  uint8_t started; // zlib's state is made
  uint8_t ended;   // the stream of data is complete
  uint8_t raw;     // deflate without the zlib wrapper, as some servers send it
  z_stream z;
  BrotliDecoderState *br;
  // What the current piece decodes to.
  uint8_t *out;
  size_t length, capacity;
} Decoder;

static void decoder_drop(void *object) {
  Decoder *d = (Decoder *)object;
  if (d->started)
    inflateEnd(&d->z);
  if (d->br)
    BrotliDecoderDestroyInstance(d->br);
  tz_free(d->out);
  tz_obj_free(d);
}

static const TzLayout decoder_layout = {decoder_drop, NULL, 0, 0, "Decoder", 7, 0};

static voidpf z_alloc(voidpf opaque, uInt items, uInt size) {
  (void)opaque;
  return tz_alloc((size_t)items * size);
}

static void z_free(voidpf opaque, voidpf address) {
  (void)opaque;
  tz_free(address);
}

static void *br_alloc(void *opaque, size_t size) {
  (void)opaque;
  return tz_alloc(size);
}

static void br_free(void *opaque, void *address) {
  (void)opaque;
  tz_free(address);
}

// `__tz_zlib_new(kind)`: a decoder of gzip (1), deflate (2) or br (3).
void *tz_zlib_new(int32_t kind, TzMaker make) {
  (void)make;
  Decoder *d = (Decoder *)tz_obj_alloc(sizeof(Decoder));
  memset(d, 0, sizeof *d);
  d->rc = 1;
  d->layout = &decoder_layout;
  d->kind = kind;
  if (kind == CODING_BR)
    d->br = BrotliDecoderCreateInstance(br_alloc, br_free, NULL);
  return d;
}

static uint8_t *room(Decoder *d, size_t *available) {
  if (d->capacity - d->length < 16384) {
    size_t cap = d->capacity * 2 + 32768;
    uint8_t *out = (uint8_t *)tz_alloc(cap);
    if (d->length)
      memcpy(out, d->out, d->length);
    tz_free(d->out);
    d->out = out;
    d->capacity = cap;
  }
  *available = d->capacity - d->length;
  return d->out + d->length;
}

// An error as node's zlib gives it: "incorrect header check".
static void fail(const char *message) {
  tz_thrown = net_error(TZ_ERROR, message, NULL);
}

static int start_zlib(Decoder *d, const uint8_t *data, size_t length) {
  // Deflate is zlib's format by its RFC, but some servers send it raw: a
  // zlib stream starts with a header whose check holds.
  if (d->kind == CODING_DEFLATE && length >= 2)
    d->raw = !((data[0] & 0x0F) == 8 && ((data[0] << 8) | data[1]) % 31 == 0);
  d->z.zalloc = z_alloc;
  d->z.zfree = z_free;
  int bits = d->kind == CODING_GZIP ? 16 + MAX_WBITS : d->raw ? -MAX_WBITS : MAX_WBITS;
  if (inflateInit2(&d->z, bits) != Z_OK) {
    fail("cannot start inflating");
    return 0;
  }
  d->started = 1;
  return 1;
}

static int feed(Decoder *d, const uint8_t *data, size_t length) {
  if (d->kind == CODING_BR) {
    size_t in = length;
    const uint8_t *next = data;
    for (;;) {
      size_t available;
      uint8_t *at = room(d, &available);
      size_t before = available;
      BrotliDecoderResult r =
          BrotliDecoderDecompressStream(d->br, &in, &next, &available, &at, NULL);
      d->length += before - available;
      if (r == BROTLI_DECODER_RESULT_ERROR) {
        fail("Decompression failed");
        return 0;
      }
      if (r == BROTLI_DECODER_RESULT_SUCCESS) {
        d->ended = 1;
        return 1;
      }
      if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT)
        return 1;
    }
  }
  if (!length)
    return 1;
  if (d->ended) // what follows the end of the data is dropped, as node does
    return 1;
  if (!d->started && !start_zlib(d, data, length))
    return 0;
  d->z.next_in = (Bytef *)data;
  d->z.avail_in = (uInt)length;
  for (;;) {
    size_t available;
    uint8_t *at = room(d, &available);
    d->z.next_out = at;
    d->z.avail_out = (uInt)available;
    int r = inflate(&d->z, Z_NO_FLUSH);
    d->length += available - d->z.avail_out;
    if (r == Z_STREAM_END) {
      // gzip may have several members, one after the other.
      if (d->kind == CODING_GZIP && d->z.avail_in >= 2 && d->z.next_in[0] == 0x1f &&
          d->z.next_in[1] == 0x8b) {
        inflateReset(&d->z);
        continue;
      }
      d->ended = 1;
      return 1;
    }
    if (r == Z_BUF_ERROR && d->z.avail_in == 0)
      return 1;
    if (r != Z_OK) {
      fail(d->z.msg ? d->z.msg : "invalid compressed data");
      return 0;
    }
    if (d->z.avail_in == 0 && d->z.avail_out != 0)
      return 1;
  }
}

static TzBytes *take(Decoder *d) {
  TzBytes *b = net_bytes(d->out, d->length);
  d->length = 0;
  return b;
}

// `__tz_zlib_push(decoder, chunk)`: what the chunk decodes to.
void *tz_zlib_push(void *decoder, void *chunk, TzMaker make) {
  (void)make;
  Decoder *d = (Decoder *)decoder;
  const TzBytes *b = (const TzBytes *)chunk;
  if (!feed(d, b->data, (size_t)tz_bytes_byte_length(b)))
    return NULL;
  return take(d);
}

// `__tz_zlib_end(decoder)`: the rest, once the data has ended; an error if
// it was cut off.
void *tz_zlib_end(void *decoder, TzMaker make) {
  (void)make;
  Decoder *d = (Decoder *)decoder;
  if (!d->ended && (d->started || d->kind == CODING_BR)) {
    fail("unexpected end of file");
    return NULL;
  }
  return take(d);
}
