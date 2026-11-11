#pragma once

#include "compiler/program/ast/builtin/builtin.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

struct BitFunctionInfo {
  llvm::StringLiteral name;
  Builtin builtin;
  unsigned bits;
  unsigned args;
  bool counts;
  bool wide;
  llvm::StringLiteral first;
  llvm::StringLiteral second;
};

class BitFunctionTable {
public:
  static const BitFunctionInfo *find(llvm::StringRef name);
};

}
