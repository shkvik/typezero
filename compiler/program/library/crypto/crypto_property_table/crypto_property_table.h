#pragma once

#include "compiler/program/types/type/type.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

enum class CryptoProperty {
  KEY_TYPE,
  KEY_ASYMMETRIC_KEY_TYPE,
  KEY_SYMMETRIC_KEY_SIZE,
  KEY_ASYMMETRIC_KEY_DETAILS,
  X509_SUBJECT,
  X509_ISSUER,
  X509_VALID_FROM,
  X509_VALID_TO,
  X509_VALID_FROM_DATE,
  X509_VALID_TO_DATE,
  X509_SERIAL_NUMBER,
  X509_FINGERPRINT,
  X509_FINGERPRINT256,
  X509_FINGERPRINT512,
  X509_SUBJECT_ALT_NAME,
  X509_INFO_ACCESS,
  X509_KEY_USAGE,
  X509_CA,
  X509_RAW,
  X509_PUBLIC_KEY,
  X509_ISSUER_CERTIFICATE,
};

struct CryptoPropertyInfo {
  llvm::StringLiteral name;
  Type::Crypto owner;
  CryptoProperty property;
};

class CryptoPropertyTable {
public:
  static const CryptoPropertyInfo *find(Type::Crypto owner, llvm::StringRef name);
};

}
