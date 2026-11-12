#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

enum class CryptoChoice {
  DSA_ENCODING,
  ARGON2_ALGORITHM,
  KEY_FORMAT,
  EXPORT_FORMAT,
  KEY_ENCODING_TYPE,
  SECRET_KEY_TYPE,
  POINT_CONVERSION,
  CHECK_SUBJECT,
  KEY_PAIR_TYPE,
};

enum class CryptoKeyFormat {
  PEM,
  DER,
  JWK,
  BUFFER,
};

enum class SecretKeyType {
  HMAC,
  AES,
};

enum class KeyPairType {
  RSA,
  RSA_PSS,
  DSA,
  EC,
  ED25519,
  ED448,
  X25519,
  X448,
};

struct CryptoChoiceInfo {
  CryptoChoice choice;
  std::vector<llvm::StringRef> values;
};

class CryptoChoiceTable {
public:
  static llvm::ArrayRef<llvm::StringRef> getValues(CryptoChoice choice);
  static bool isBufferEncoding(llvm::StringRef text);
};

}
