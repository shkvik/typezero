#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class SubtleOperation {
  DIGEST,
  GENERATE_KEY,
  IMPORT_KEY,
  ENCRYPT,
  DECRYPT,
  SIGN,
  VERIFY,
  DERIVE_BITS,
  WRAP_KEY,
  UNWRAP_KEY,
  KEY_LENGTH,
};

struct SubtleOperationInfo {
  llvm::StringLiteral name;
  SubtleOperation value;
};

class SubtleOperationTable {
public:
  static std::optional<SubtleOperation> find(llvm::StringRef name);
  static llvm::StringRef getName(SubtleOperation value);
};

}
