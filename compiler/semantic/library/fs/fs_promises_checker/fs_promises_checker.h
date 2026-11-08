#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/fs/fs_promises_checker/fs_promises_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class FsPromisesChecker {
public:
  explicit FsPromisesChecker(SemanticState &owner);

  Expr *checkFsPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  using ResultPromiseFunction = Result<llvm::StringRef, FsPromisesCheckerPromiseFunctionError>;

  static ResultPromiseFunction promiseFunction(llvm::StringRef fn);

  SemanticState &state;
};

}
