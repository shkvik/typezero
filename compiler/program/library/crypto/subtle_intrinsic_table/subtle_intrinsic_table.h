#pragma once

#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

struct SubtleIntrinsic {
  llvm::StringLiteral name;
  CryptoFn fn;
  llvm::StringLiteral params;
  char result;
};

class SubtleIntrinsicTable {
public:
  static const SubtleIntrinsic *find(llvm::StringRef name);
  static bool isSubtleName(llvm::StringRef name);
};

}
