#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

struct GlobalConstantInfo {
  GlobalName owner;
  llvm::StringLiteral name;
  double value;
};

class GlobalConstantTable {
public:
  static std::optional<double> find(GlobalName owner, llvm::StringRef name);
};

}
