#pragma once

#include "compiler/program/ast/builtin/builtin.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

struct DecimalStaticInfo {
  llvm::StringLiteral name;
  Builtin builtin;
};

class DecimalStaticTable {
public:
  static const DecimalStaticInfo *find(llvm::StringRef name);
};

}
