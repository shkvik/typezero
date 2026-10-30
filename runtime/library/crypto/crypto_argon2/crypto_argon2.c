#include "runtime/library/crypto/crypto/crypto.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/error/error.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include <string.h>

typedef struct {
  uint8_t digest_length, key_length, fanout, depth, leaf_length[4];
  uint8_t node_offset[8], node_depth, inner_length, reserved[14], salt[16], personal[16];
} Blake2Param;
typedef struct {
  uint64_t h[8], t[2], f[2];
  uint8_t buf[128];
  size_t buflen, outlen;
} Blake2bCtx;
void ossl_blake2b_param_init(Blake2Param *p);
void ossl_blake2b_param_set_digest_length(Blake2Param *p, uint8_t length);
int ossl_blake2b_init(Blake2bCtx *c, const Blake2Param *p);
int ossl_blake2b_update(Blake2bCtx *c, const void *data, size_t n);
int ossl_blake2b_final(unsigned char *md, Blake2bCtx *c);

enum { BLOCK_WORDS = 128, BLOCK_BYTES = 1024, SYNC_POINTS = 4 };

typedef struct {
  uint64_t v[BLOCK_WORDS];
} Block;

static void blake2b_start(Blake2bCtx *c, size_t length) {
  Blake2Param p;
  ossl_blake2b_param_init(&p);
  ossl_blake2b_param_set_digest_length(&p, (uint8_t)length);
  ossl_blake2b_init(c, &p);
}

static void put32(Blake2bCtx *c, uint32_t x) {
  uint8_t b[4] = {(uint8_t)x, (uint8_t)(x >> 8), (uint8_t)(x >> 16), (uint8_t)(x >> 24)};
  ossl_blake2b_update(c, b, 4);
}

static void hash_long(uint8_t *out, size_t length, const uint8_t *in, size_t in_length) {
  Blake2bCtx c;
  blake2b_start(&c, length <= 64 ? length : 64);
  put32(&c, (uint32_t)length);
  ossl_blake2b_update(&c, in, in_length);
  if (length <= 64) {
    ossl_blake2b_final(out, &c);
    return;
  }
  uint8_t v[64];
  ossl_blake2b_final(v, &c);
  memcpy(out, v, 32);
  out += 32;
  size_t left = length - 32;
  while (left > 64) {
    blake2b_start(&c, 64);
    ossl_blake2b_update(&c, v, 64);
    ossl_blake2b_final(v, &c);
    memcpy(out, v, 32);
    out += 32;
    left -= 32;
  }
  blake2b_start(&c, left);
  ossl_blake2b_update(&c, v, 64);
  ossl_blake2b_final(out, &c);
}

static uint64_t rotr(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static uint64_t blamka(uint64_t x, uint64_t y) {
  return x + y + 2 * (x & 0xffffffffu) * (y & 0xffffffffu);
}

#define G(a, b, c, d)                                                                              \
  do {                                                                                             \
    a = blamka(a, b);                                                                              \
    d = rotr(d ^ a, 32);                                                                           \
    c = blamka(c, d);                                                                              \
    b = rotr(b ^ c, 24);                                                                           \
    a = blamka(a, b);                                                                              \
    d = rotr(d ^ a, 16);                                                                           \
    c = blamka(c, d);                                                                              \
    b = rotr(b ^ c, 63);                                                                           \
  } while (0)

static void permute(uint64_t *v, const int at[16]) {
  uint64_t *x[16];
  for (int i = 0; i < 16; ++i) {
    x[i] = &v[at[i]];
  }
  G(*x[0], *x[4], *x[8], *x[12]);
  G(*x[1], *x[5], *x[9], *x[13]);
  G(*x[2], *x[6], *x[10], *x[14]);
  G(*x[3], *x[7], *x[11], *x[15]);
  G(*x[0], *x[5], *x[10], *x[15]);
  G(*x[1], *x[6], *x[11], *x[12]);
  G(*x[2], *x[7], *x[8], *x[13]);
  G(*x[3], *x[4], *x[9], *x[14]);
}

static void fill_block(const Block *prev, const Block *ref, Block *next, int with_xor) {
  Block r, t;
  for (int i = 0; i < BLOCK_WORDS; ++i) {
    r.v[i] = ref->v[i] ^ prev->v[i];
  }
  t = r;
  if (with_xor) {
    for (int i = 0; i < BLOCK_WORDS; ++i) {
      t.v[i] ^= next->v[i];
    }
  }
  int at[16];
  for (int i = 0; i < 8; ++i) {
    for (int k = 0; k < 16; ++k) {
      at[k] = 16 * i + k;
    }
    permute(r.v, at);
  }
  for (int i = 0; i < 8; ++i) {
    for (int k = 0; k < 8; ++k) {
      at[2 * k] = 2 * i + 16 * k;
      at[2 * k + 1] = 2 * i + 16 * k + 1;
    }
    permute(r.v, at);
  }
  for (int i = 0; i < BLOCK_WORDS; ++i) {
    next->v[i] = t.v[i] ^ r.v[i];
  }
}

typedef struct {
  Block *memory;
  uint32_t lanes, lane_length, segment_length, memory_blocks, passes, type;
} Instance;

static void next_addresses(Block *address, Block *input, const Block *zero) {
  ++input->v[6];
  fill_block(zero, input, address, 0);
  fill_block(zero, address, address, 0);
}

static uint32_t index_alpha(const Instance *in, uint32_t pass, uint32_t slice, uint32_t index,
                            uint32_t pseudo_rand, int same_lane) {
  uint32_t area;
  if (pass == 0) {
    if (slice == 0) {
      area = index - 1;
    } else if (same_lane) {
      area = slice * in->segment_length + index - 1;
    } else {
      area = slice * in->segment_length + (index == 0 ? (uint32_t)-1 : 0);
    }
  } else if (same_lane) {
    area = in->lane_length - in->segment_length + index - 1;
  } else {
    area = in->lane_length - in->segment_length + (index == 0 ? (uint32_t)-1 : 0);
  }
  uint64_t relative = pseudo_rand;
  relative = relative * relative >> 32;
  relative = area - 1 - ((uint64_t)area * relative >> 32);
  uint32_t start = 0;
  if (pass != 0) {
    start = slice == SYNC_POINTS - 1 ? 0 : (slice + 1) * in->segment_length;
  }

  return (uint32_t)((start + relative) % in->lane_length);
}

static void fill_segment(const Instance *in, uint32_t pass, uint32_t lane, uint32_t slice) {
  int independent = in->type == 1 || (in->type == 2 && pass == 0 && slice < SYNC_POINTS / 2);
  Block address, input, zero;
  if (independent) {
    memset(&zero, 0, sizeof zero);
    memset(&input, 0, sizeof input);
    input.v[0] = pass;
    input.v[1] = lane;
    input.v[2] = slice;
    input.v[3] = in->memory_blocks;
    input.v[4] = in->passes;
    input.v[5] = in->type;
  }
  uint32_t start = 0;
  if (pass == 0 && slice == 0) {
    start = 2;
    if (independent) {
      next_addresses(&address, &input, &zero);
    }
  }
  uint32_t current = lane * in->lane_length + slice * in->segment_length + start;
  uint32_t previous = current % in->lane_length == 0 ? current + in->lane_length - 1 : current - 1;
  for (uint32_t i = start; i < in->segment_length; ++i, ++current, ++previous) {
    if (current % in->lane_length == 1) {
      previous = current - 1;
    }
    uint64_t pseudo_rand;
    if (independent) {
      if (i % BLOCK_WORDS == 0) {
        next_addresses(&address, &input, &zero);
      }
      pseudo_rand = address.v[i % BLOCK_WORDS];
    } else {
      pseudo_rand = in->memory[previous].v[0];
    }
    uint64_t ref_lane = (pseudo_rand >> 32) % in->lanes;
    if (pass == 0 && slice == 0) {
      ref_lane = lane;
    }
    uint32_t ref_index = index_alpha(in, pass, slice, i, (uint32_t)pseudo_rand, ref_lane == lane);
    const Block *ref = in->memory + in->lane_length * ref_lane + ref_index;
    fill_block(in->memory + previous, ref, in->memory + current, pass != 0);
  }
}

static void load_block(Block *b, const uint8_t *bytes) {
  for (int i = 0; i < BLOCK_WORDS; ++i) {
    uint64_t w = 0;
    for (int k = 7; k >= 0; --k) {
      w = w << 8 | bytes[8 * i + k];
    }
    b->v[i] = w;
  }
}

static void store_block(uint8_t *bytes, const Block *b) {
  for (int i = 0; i < BLOCK_WORDS; ++i) {
    for (int k = 0; k < 8; ++k) {
      bytes[8 * i + k] = (uint8_t)(b->v[i] >> (8 * k));
    }
  }
}

static void put_data(Blake2bCtx *c, const TzCryptoData *d) {
  put32(c, (uint32_t)d->length);
  if (d->length) {
    ossl_blake2b_update(c, d->data, d->length);
  }
}

TzBytes *tz_crypto_argon2(int32_t type, void *name_obj, uint64_t name_meta, const TzType *mt,
                          const void *mv, const TzType *nt, const void *nv, double parallelism,
                          double tag_length, double memory, double passes, const TzType *st,
                          const void *sv, const TzType *at, const void *av) {
  if (type < 0) {
    char name[16];
    tz_crypto_cstr(name_obj, name_meta, name, sizeof name);
    type = strcmp(name, "argon2d") == 0    ? 0
           : strcmp(name, "argon2i") == 0  ? 1
           : strcmp(name, "argon2id") == 0 ? 2
                                           : -1;
    if (type < 0) {
      tz_crypto_note_algorithm(name);
      tz_crypto_fail("ERR_INVALID_ARG_VALUE",
                     "there is no algorithm \"%s\"; use argon2d, argon2i or argon2id", name);
      return NULL;
    }
  }
  TzCryptoData message, nonce, secret, data;
  if (!tz_crypto_data(&message, mt, mv, TZ_ENC_UTF8, "parameters.message")) {
    return NULL;
  }
  if (!tz_crypto_data(&nonce, nt, nv, TZ_ENC_UTF8, "parameters.nonce")) {
    tz_crypto_data_done(&message);
    return NULL;
  }
  tz_crypto_data(&secret, NULL, NULL, TZ_ENC_UTF8, "");
  tz_crypto_data(&data, NULL, NULL, TZ_ENC_UTF8, "");
  TzBytes *result = NULL;
  if (!tz_crypto_data(&secret, st, sv, TZ_ENC_UTF8, "parameters.secret") ||
      !tz_crypto_data(&data, at, av, TZ_ENC_UTF8, "parameters.associatedData")) {
    goto done;
  }
  if (nonce.length < 8) {
    tz_crypto_note_parameter("nonce", (double)nonce.length);
    tz_crypto_fail("ERR_OUT_OF_RANGE", "argon2: the nonce must have 8 bytes or more, not %zu",
                   nonce.length);
    goto done;
  }
  if (!tz_crypto_check_integer(parallelism, "parameters.parallelism", 1, 16777215) ||
      !tz_crypto_check_integer(tag_length, "parameters.tagLength", 4, 4294967295.0) ||
      !tz_crypto_check_integer(memory, "parameters.memory", 8 * parallelism, 4294967295.0) ||
      !tz_crypto_check_integer(passes, "parameters.passes", 1, 4294967295.0)) {
    goto done;
  }
  if (!(result = tz_buffer_alloc((int64_t)tag_length, 0))) {
    goto done;
  }
  {
    uint32_t p = (uint32_t)parallelism, m = (uint32_t)memory, t = (uint32_t)passes;
    uint32_t tag = (uint32_t)tag_length;
    Blake2bCtx c;
    uint8_t h0[72];
    blake2b_start(&c, 64);
    put32(&c, p);
    put32(&c, tag);
    put32(&c, m);
    put32(&c, t);
    put32(&c, 0x13);
    put32(&c, (uint32_t)type);
    put_data(&c, &message);
    put_data(&c, &nonce);
    put_data(&c, &secret);
    put_data(&c, &data);
    ossl_blake2b_final(h0, &c);

    Instance in;
    uint32_t blocks = m < 2 * SYNC_POINTS * p ? 2 * SYNC_POINTS * p : m;
    in.segment_length = blocks / (p * SYNC_POINTS);
    in.memory_blocks = in.segment_length * p * SYNC_POINTS;
    in.lane_length = in.segment_length * SYNC_POINTS;
    in.lanes = p;
    in.passes = t;
    in.type = (uint32_t)type;
    in.memory = (Block *)tz_alloc((size_t)in.memory_blocks * sizeof(Block));
    uint8_t bytes[BLOCK_BYTES];
    for (uint32_t lane = 0; lane < p; ++lane) {
      for (uint32_t k = 0; k < 2; ++k) {
        for (int i = 0; i < 4; ++i) {
          h0[64 + i] = (uint8_t)(k >> (8 * i));
          h0[68 + i] = (uint8_t)(lane >> (8 * i));
        }
        hash_long(bytes, BLOCK_BYTES, h0, sizeof h0);
        load_block(&in.memory[lane * in.lane_length + k], bytes);
      }
    }
    for (uint32_t pass = 0; pass < t; ++pass) {
      for (uint32_t slice = 0; slice < SYNC_POINTS; ++slice) {
        for (uint32_t lane = 0; lane < p; ++lane) {
          fill_segment(&in, pass, lane, slice);
        }
      }
    }
    Block final = in.memory[in.lane_length - 1];
    for (uint32_t lane = 1; lane < p; ++lane) {
      for (int i = 0; i < BLOCK_WORDS; ++i) {
        final.v[i] ^= in.memory[lane * in.lane_length + in.lane_length - 1].v[i];
      }
    }
    store_block(bytes, &final);
    hash_long(result->data, tag, bytes, sizeof bytes);
    memset(in.memory, 0, (size_t)in.memory_blocks * sizeof(Block));
    tz_free(in.memory);
    memset(bytes, 0, sizeof bytes);
    memset(h0, 0, sizeof h0);
  }
done:
  tz_crypto_data_done(&data);
  tz_crypto_data_done(&secret);
  tz_crypto_data_done(&nonce);
  tz_crypto_data_done(&message);

  return result;
}
