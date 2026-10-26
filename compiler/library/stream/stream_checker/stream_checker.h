#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class StreamChecker {
public:
  explicit StreamChecker(Checker &checker);

  Expr *checkStreamIntrinsic(CallExpr *call, NameExpr *name, const Type *typeArg);
  const Type *streamCallbackType();
  Expr *streamChunkArg(Expr *arg, const Type *chunk, bool eof, bool writing, llvm::StringRef method,
                       bool checked);
  Expr *checkStreamCall(CallExpr *call, MemberExpr *member, ClassDecl *cls);

private:
  Checker &checker_;
};

}
