#pragma once

#include "compiler/program/ast/builtin/builtin.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

enum class PathFunctionForm {
  JOINED,
  TEXT,
  TEST,
  PARSED,
  FORMAT,
};

struct PathFunctionInfo {
  llvm::StringLiteral name;
  Builtin builtin;
  PathFunctionForm form;
  unsigned min;
  unsigned max;
};

class PathFunctionTable {
public:
  static const PathFunctionInfo *find(llvm::StringRef name);
};

}
