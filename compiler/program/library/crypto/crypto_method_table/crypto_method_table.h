#pragma once

#include "compiler/program/types/type/type.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

enum class CryptoMethod {
  HASH_UPDATE,
  DIGEST,
  HASH_COPY,
  SIGN,
  VERIFY,
  CIPHER_UPDATE,
  CIPHER_FINAL,
  SET_AAD,
  GET_AUTH_TAG,
  SET_AUTH_TAG,
  SET_AUTO_PADDING,
  KEY_EXPORT,
  KEY_EQUALS,
  ECDH_GENERATE_KEYS,
  ECDH_GET_PUBLIC_KEY,
  ECDH_GET_PRIVATE_KEY,
  ECDH_SET_PRIVATE_KEY,
  ECDH_COMPUTE_SECRET,
  X509_PEM,
  X509_CHECK_HOST,
  X509_CHECK_EMAIL,
  X509_CHECK_IP,
  X509_CHECK_ISSUED,
  X509_CHECK_PRIVATE_KEY,
  X509_VERIFY,
};

struct CryptoMethodInfo {
  llvm::StringLiteral name;
  Type::Crypto owner;
  CryptoMethod method;
};

class CryptoMethodTable {
public:
  static const CryptoMethodInfo *find(Type::Crypto owner, llvm::StringRef name);
};

}
