#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The functions of node:path.
class PathChecker {
public:
  explicit PathChecker(Sema &sema);

  // The calls of the functions of each module, by name.
  Expr *checkPathCall(CallExpr *call, NodeModule module, llvm::StringRef fn, llvm::SMLoc loc);

private:
  Sema &sema_;
};

} // namespace tz
