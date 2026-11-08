#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/stream/stream_new_checker/stream_new_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class StreamNewChecker {
public:
  explicit StreamNewChecker(SemanticState &owner);

  Expr *checkStreamNew(NewExpr *expr, ClassDecl *generic, const Type *expected);

private:
  using ResultConcreteStream = Result<llvm::StringRef, StreamNewCheckerConcreteStreamError>;

  static ResultConcreteStream concreteStream(llvm::StringRef name, bool readable, bool writable);

  SemanticState &state;
};

}
