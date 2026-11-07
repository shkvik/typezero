#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class StreamSources {
public:
  explicit StreamSources(Checker &owner);

  Expr *checkFsStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc);
  Expr *processStream(MemberExpr *member);
  VarSymbol *processStdinVar();
  const Type *streamFromOptionsType();
  Expr *checkStreamFrom(CallExpr *call, MemberExpr *member);

private:
  Checker &checker;
  VarSymbol *processStdin = nullptr;
};

}
