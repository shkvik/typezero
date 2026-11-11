#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class PromiseStatic {
  RESOLVE,
  REJECT,
  ALL_SETTLED,
  ANY,
  WITH_RESOLVERS,
  ALL,
  RACE,
};

struct PromiseStaticInfo {
  llvm::StringLiteral name;
  PromiseStatic value;
};

class PromiseStaticTable {
public:
  static std::optional<PromiseStatic> find(llvm::StringRef name);
};

}
