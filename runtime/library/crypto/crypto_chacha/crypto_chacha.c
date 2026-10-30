#include "runtime/library/crypto/crypto/crypto.h"
#include "runtime/core/error/error.h"
#include <stddef.h>
#include <stdint.h>
#include <openssl/crypto.h>
#include <string.h>
#include "crypto/poly1305.h"

void ChaCha20_ctr32(unsigned char *out, const unsigned char *inp, size_t len,
                    const unsigned int key[8], const unsigned int counter[4]);

typedef struct {
  unsigned int key[8];
  unsigned int counter[4];
  uint8_t stream[64];
  unsigned int used;
  POLY1305 poly;
  uint64_t aad_length, data_length;
  int data;
} Chacha;

static unsigned int le32(const uint8_t *p) {
  return (unsigned int)p[0] | (unsigned int)p[1] << 8 | (unsigned int)p[2] << 16 |
         (unsigned int)p[3] << 24;
}

static int chacha_init(TzCipher *c, const uint8_t *key, size_t key_length, const uint8_t *iv,
                       size_t iv_length) {
  (void)key_length;
  (void)iv_length;
  Chacha *k = (Chacha *)c->ctx;
  for (int i = 0; i < 8; ++i) {
    k->key[i] = le32(key + 4 * i);
  }
  k->counter[0] = 0;
  for (int i = 0; i < 3; ++i) {
    k->counter[1 + i] = le32(iv + 4 * i);
  }
  uint8_t block[64] = {0};
  ChaCha20_ctr32(block, block, sizeof block, k->key, k->counter);
  Poly1305_Init(&k->poly, block);
  memset(block, 0, sizeof block);
  k->counter[0] = 1;

  return 1;
}

static void pad16(Chacha *k, uint64_t length) {
  static const uint8_t zeros[16] = {0};
  if (length % 16) {
    Poly1305_Update(&k->poly, zeros, 16 - (size_t)(length % 16));
  }
}

static int chacha_aad(TzCipher *c, const uint8_t *aad, size_t length, double plaintext_length) {
  (void)plaintext_length;
  Chacha *k = (Chacha *)c->ctx;
  Poly1305_Update(&k->poly, aad, length);
  k->aad_length += length;

  return 1;
}

static void chacha_xor(Chacha *k, const uint8_t *in, size_t length, uint8_t *out) {
  if (k->used) {
    while (length > 0 && k->used < 64) {
      *out++ = *in++ ^ k->stream[k->used++];
      --length;
    }
    if (length == 0) {
      return;
    }
    k->used = 0;
    ++k->counter[0];
  }
  size_t rest = length % 64;
  length -= rest;
  while (length >= 64) {
    size_t blocks = length / 64;
    if (blocks > (1u << 28)) {
      blocks = 1u << 28;
    }
    unsigned int counter = k->counter[0] + (unsigned int)blocks;
    if (counter < blocks) {
      blocks -= counter;
      counter = 0;
    }
    ChaCha20_ctr32(out, in, blocks * 64, k->key, k->counter);
    in += blocks * 64;
    out += blocks * 64;
    length -= blocks * 64;
    k->counter[0] = counter;
  }
  if (rest) {
    memset(k->stream, 0, sizeof k->stream);
    ChaCha20_ctr32(k->stream, k->stream, 64, k->key, k->counter);
    for (size_t i = 0; i < rest; ++i) {
      out[i] = in[i] ^ k->stream[i];
    }
    k->used = (unsigned int)rest;
  }
}

static int64_t chacha_update(TzCipher *c, const uint8_t *in, size_t length, uint8_t *out) {
  Chacha *k = (Chacha *)c->ctx;
  if (!k->data) {
    pad16(k, k->aad_length);
    k->data = 1;
  }
  if (c->decrypt) {
    Poly1305_Update(&k->poly, in, length);
  }
  chacha_xor(k, in, length, out);
  if (!c->decrypt) {
    Poly1305_Update(&k->poly, out, length);
  }
  k->data_length += length;

  return (int64_t)length;
}

static int64_t chacha_final(TzCipher *c, uint8_t *out) {
  (void)out;
  Chacha *k = (Chacha *)c->ctx;
  if (!k->data) {
    pad16(k, k->aad_length);
    k->data = 1;
  }
  pad16(k, k->data_length);
  uint8_t lengths[16];
  for (int i = 0; i < 8; ++i) {
    lengths[i] = (uint8_t)(k->aad_length >> (8 * i));
    lengths[8 + i] = (uint8_t)(k->data_length >> (8 * i));
  }
  Poly1305_Update(&k->poly, lengths, sizeof lengths);
  uint8_t mac[16];
  Poly1305_Final(&k->poly, mac);
  if (c->decrypt) {
    int ok = CRYPTO_memcmp(mac, c->tag, (size_t)c->tag_length) == 0;
    memset(mac, 0, sizeof mac);
    if (!ok) {
      tz_crypto_auth_failed(c);
      return -1;
    }
  } else {
    memcpy(c->tag, mac, sizeof mac);
  }

  return 0;
}

static const TzCipherOps chacha_ops = {chacha_init, chacha_aad, chacha_update, chacha_final, NULL};

const TzCipherAlg tz_crypto_cipher_chacha20_poly1305 = {
    &chacha_ops, "chacha20-poly1305", sizeof(Chacha), 32, 12, 1, TZ_CIPHER_AEAD, 0, NULL};
