#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class X509Static {
  PARSE,
};

struct X509StaticInfo {
  llvm::StringLiteral name;
  X509Static value;
};

class X509StaticTable {
public:
  static std::optional<X509Static> find(llvm::StringRef name);
};

}
