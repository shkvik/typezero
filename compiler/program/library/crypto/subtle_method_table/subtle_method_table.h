#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class SubtleMethod {
  DIGEST,
  GENERATE_KEY,
  IMPORT_KEY,
  EXPORT_KEY,
  SIGN,
  VERIFY,
  ENCRYPT,
  DECRYPT,
  DERIVE_BITS,
  DERIVE_KEY,
  WRAP_KEY,
  UNWRAP_KEY,
};

struct SubtleMethodInfo {
  llvm::StringLiteral name;
  SubtleMethod method;
};

class SubtleMethodTable {
public:
  static std::optional<SubtleMethod> find(llvm::StringRef name);
};

}
