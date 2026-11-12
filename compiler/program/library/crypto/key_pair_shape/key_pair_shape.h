#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

enum class KeyPairField {
  PUBLIC_KEY,
  PRIVATE_KEY,
};

struct KeyPairFieldInfo {
  llvm::StringLiteral name;
  KeyPairField field;
};

class KeyPairShape {
public:
  static llvm::StringRef getTypeName();
  static llvm::StringRef getFieldName(KeyPairField field);
};

}
