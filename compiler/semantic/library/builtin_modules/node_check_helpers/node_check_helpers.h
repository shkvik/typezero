#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class NodeCheckHelpers {
public:
  static bool isTargetWindows();
  static Expr *stringLiteral(AstContext &context, llvm::SMLoc loc, llvm::StringRef text);
};

}
