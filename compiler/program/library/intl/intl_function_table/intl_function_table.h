#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class IntlFunction {
  GET_CANONICAL_LOCALES,
  SUPPORTED_VALUES_OF,
  SUPPORTED_LOCALES_OF,
};

struct IntlFunctionInfo {
  llvm::StringLiteral name;
  IntlFunction value;
};

class IntlFunctionTable {
public:
  static std::optional<IntlFunction> find(llvm::StringRef name);
};

}
