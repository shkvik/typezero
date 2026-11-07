#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/stream/stream_new_checker/stream_new_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class StreamNewChecker {
public:
  explicit StreamNewChecker(CheckState &owner);

  Expr *checkStreamNew(NewExpr *expr, ClassDecl *generic, const Type *expected);

private:
  using ResultConcreteStream = Result<llvm::StringRef, StreamNewCheckerConcreteStreamError>;

  static ResultConcreteStream concreteStream(llvm::StringRef name, bool readable, bool writable);

  CheckState &state;
};

}
