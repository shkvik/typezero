#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class FsPromisesChecker {
public:
  explicit FsPromisesChecker(CheckState &owner);

  Expr *checkFsPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  CheckState &state;
};

}
