#pragma once

#include "compiler/program/library/crypto/subtle_key_usage_table/subtle_key_usage_table.h"
#include "compiler/program/library/crypto/subtle_operation_table/subtle_operation_table.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

enum class SubtleAlgorithm {
  AES_CBC,
  AES_CTR,
  AES_GCM,
  AES_KW,
  ECDH,
  ECDSA,
  ED25519,
  HKDF,
  HMAC,
  PBKDF2,
  RSA_OAEP,
  RSA_PSS,
  RSASSA_PKCS1_V1_5,
  SHA_1,
  SHA_256,
  SHA_384,
  SHA_512,
  X25519,
};

enum class SubtleFamily {
  DIGEST,
  HMAC,
  AES,
  KDF,
  EC,
  RSA,
  OKP,
};

struct SubtleAlgorithmInfo {
  llvm::StringLiteral name;
  SubtleAlgorithm algorithm;
  SubtleFamily family;
  std::vector<SubtleOperation> operations;
  std::vector<SubtleKeyUsage> publicUsages;
  std::vector<SubtleKeyUsage> privateUsages;
};

class SubtleAlgorithmTable {
public:
  static const SubtleAlgorithmInfo *findUpper(llvm::StringRef upper);
  static const SubtleAlgorithmInfo *findIn(SubtleOperation operation, llvm::StringRef upper);
  static std::vector<llvm::StringRef> getNames(SubtleOperation operation);
  static bool isExperimental(llvm::StringRef upper);
  static bool hasShaPrefix(llvm::StringRef name);
};

}
