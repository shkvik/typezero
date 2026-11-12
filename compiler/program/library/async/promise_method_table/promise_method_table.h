#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class PromiseMethod {
  THEN,
  CATCH,
  FINALLY,
};

struct PromiseMethodInfo {
  llvm::StringLiteral name;
  PromiseMethod value;
};

class PromiseMethodTable {
public:
  static std::optional<PromiseMethod> find(llvm::StringRef name);
};

}
