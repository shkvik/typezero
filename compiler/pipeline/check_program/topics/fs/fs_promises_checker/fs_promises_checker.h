#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/fs/fs_promises_checker/fs_promises_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class FsPromisesChecker {
public:
  explicit FsPromisesChecker(CheckState &owner);

  Expr *checkFsPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  using ResultPromiseFunction = Result<llvm::StringRef, FsPromisesCheckerPromiseFunctionError>;

  static ResultPromiseFunction promiseFunction(llvm::StringRef fn);

  CheckState &state;
};

}
