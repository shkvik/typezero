#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/stream/stream_sources/stream_sources_errors.h"
#include "compiler/result/result/result.h"

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
  using ResultValuesIterator = Result<Expr *, StreamSourcesValuesIteratorError>;

  ResultValuesIterator valuesIterator(Expr *source);

  CheckState &state;
};

}
