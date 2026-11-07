#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class ReadlineChecker {
public:
  explicit ReadlineChecker(CheckState &owner);

  Expr *checkReadlineCall(CallExpr *call, NodeModule module, llvm::StringRef fn, llvm::SMLoc loc);

private:
  CheckState &state;
};

}
