#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class PromiseResolversField {
  PROMISE,
  RESOLVE,
};

struct PromiseResolversFieldInfo {
  llvm::StringLiteral name;
  PromiseResolversField value;
};

class PromiseResolversFieldTable {
public:
  static std::optional<PromiseResolversField> find(llvm::StringRef name);
  static llvm::StringRef getName(PromiseResolversField value);
};

}
