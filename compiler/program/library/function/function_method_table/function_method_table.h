#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class FunctionMethod {
  CALL,
  APPLY,
  BIND,
};

struct FunctionMethodInfo {
  llvm::StringLiteral name;
  FunctionMethod method;
};

class FunctionMethodTable {
public:
  static std::optional<FunctionMethod> find(llvm::StringRef name);
  static llvm::StringRef getName(FunctionMethod method);
};

}
