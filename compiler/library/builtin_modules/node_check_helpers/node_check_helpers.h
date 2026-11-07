#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class NodeCheckHelpers {
public:
  static std::string moduleLabel(NodeModule module);
  static bool targetIsWindows();
  static Expr *stringLiteral(AstContext &ctx, llvm::SMLoc loc, llvm::StringRef text);
};

}
