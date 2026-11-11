#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class CryptoSyncFunction {
  CONSTANTS,
  CREATE_HASH,
  CREATE_HMAC,
  HASH,
  RANDOM_BYTES,
  RANDOM_UUID,
  RANDOM_INT,
  RANDOM_FILL_SYNC,
  GET_RANDOM_VALUES,
  TIMING_SAFE_EQUAL,
  PBKDF2_SYNC,
  HKDF_SYNC,
  SCRYPT_SYNC,
  ARGON2_SYNC,
  CREATE_CIPHERIV,
  CREATE_DECIPHERIV,
  GET_HASHES,
  GET_CIPHERS,
  GET_CURVES,
  CREATE_SECRET_KEY,
  CREATE_PUBLIC_KEY,
  CREATE_PRIVATE_KEY,
  GENERATE_KEY_PAIR_SYNC,
  GENERATE_KEY_SYNC,
  SIGN,
  VERIFY,
  CREATE_SIGN,
  CREATE_VERIFY,
  CREATE_ECDH,
  DIFFIE_HELLMAN,
  GENERATE_PRIME_SYNC,
  CHECK_PRIME_SYNC,
};

struct CryptoSyncFunctionInfo {
  llvm::StringLiteral name;
  CryptoSyncFunction function;
};

class CryptoSyncFunctionTable {
public:
  static std::optional<CryptoSyncFunction> find(llvm::StringRef name);
};

}
