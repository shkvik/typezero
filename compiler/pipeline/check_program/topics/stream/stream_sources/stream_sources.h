#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class StreamSources {
public:
  explicit StreamSources(CheckState &owner);

  Expr *checkFsStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc);
  Expr *processStream(MemberExpr *member);
  VarSymbol *processStdinVar();
  const Type *streamFromOptionsType();
  Expr *checkStreamFrom(CallExpr *call, MemberExpr *member);

private:
  CheckState &state;
};

}
