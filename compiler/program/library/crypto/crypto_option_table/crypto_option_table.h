#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class CryptoOption {
  KEY,
  PADDING,
  SALT_LENGTH,
  DSA_ENCODING,
  OUTPUT_LENGTH,
  DISABLE_ENTROPY_CACHE,
  SCRYPT_N,
  SCRYPT_R,
  SCRYPT_P,
  MAX_MEMORY,
  COST,
  BLOCK_SIZE,
  PARALLELIZATION,
  MESSAGE,
  NONCE,
  PARALLELISM,
  TAG_LENGTH,
  MEMORY,
  PASSES,
  SECRET,
  ASSOCIATED_DATA,
  AUTH_TAG_LENGTH,
  FORMAT,
  TYPE,
  PASSPHRASE,
  ENCODING,
  LENGTH,
  PRIVATE_KEY,
  PUBLIC_KEY,
  ADDEND,
  REMAINDER,
  SAFE,
  BIGINT,
  CHECKS,
  PLAINTEXT_LENGTH,
  CIPHER,
  SUBJECT,
  WILDCARDS,
  PARTIAL_WILDCARDS,
  MULTI_LABEL_WILDCARDS,
  SINGLE_LABEL_SUBDOMAINS,
  MODULUS_LENGTH,
  PUBLIC_EXPONENT,
  NAMED_CURVE,
  PUBLIC_KEY_ENCODING,
  PRIVATE_KEY_ENCODING,
  DIVISOR_LENGTH,
  HASH_ALGORITHM,
  MGF1_HASH_ALGORITHM,
};

struct CryptoOptionInfo {
  llvm::StringLiteral name;
  CryptoOption value;
};

class CryptoOptionTable {
public:
  static std::optional<CryptoOption> find(llvm::StringRef name);
  static llvm::StringRef getName(CryptoOption value);
};

}
