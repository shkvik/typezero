#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class WeakRefMethod {
  DEREF,
};

struct WeakRefMethodInfo {
  llvm::StringLiteral name;
  WeakRefMethod method;
};

class WeakRefMethodTable {
public:
  static std::optional<WeakRefMethod> find(llvm::StringRef name);
};

}
