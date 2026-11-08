#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/stream/stream_sources/stream_sources_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class StreamSources {
public:
  explicit StreamSources(SemanticState &owner);

  Expr *checkFsStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc);
  Expr *processStream(MemberExpr *member);
  VarSymbol *processStdinVar();
  const Type *streamFromOptionsType();
  Expr *checkStreamFrom(CallExpr *call, MemberExpr *member);

private:
  using ResultValuesIterator = Result<Expr *, StreamSourcesValuesIteratorError>;

  ResultValuesIterator valuesIterator(Expr *source);

  SemanticState &state;
};

}
