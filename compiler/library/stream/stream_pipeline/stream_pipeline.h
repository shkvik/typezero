#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class StreamPipeline {
public:
  explicit StreamPipeline(Checker &checker);

  Expr *checkStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc);
  Expr *checkLibraryCall(CallExpr *call, NameExpr *name, FuncSymbol *func);
  const Type *streamSignalOptionsType();
  Expr *streamStage(Expr *&arg, const Type *&chunk, bool first, bool last, bool &stream,
                    const Type **sinkValue);
  bool streamTakes(const Type *writable, const Type *chunk);
  Expr *valuesIteratorOf(Expr *collection);

private:
  Checker &checker_;
};

}
