#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class NodeCheckHelpers {
public:
  static std::string moduleLabel(NodeModule module);
  static bool targetIsWindows();
  static Expr *stringLiteral(AstContext &context, llvm::SMLoc loc, llvm::StringRef text);
};

}
