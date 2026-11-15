#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"

namespace tz {

class NodeCheckHelpers {
public:
  static bool isTargetWindows();
  static Expr *stringLiteral(AstContext &context, llvm::SMLoc loc, llvm::StringRef text);
};

}
