#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// pipeline and the functions of node:stream/promises.
class StreamPipeline {
public:
  explicit StreamPipeline(Checker &checker);

  // pipeline(), finished() and compose() of node:stream, and pipeline() of
  // node:stream/promises, which `decl` declares; null for other functions.
  Expr *checkStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc);
  // A call of `func` when it is pipeline() or another function of node:stream.
  Expr *checkLibraryCall(CallExpr *call, NameExpr *name, FuncSymbol *func);
  // `{ signal: AbortSignal }`, what the functions of pipeline() get.
  const Type *streamSignalOptionsType();
  // A stage of pipeline(), checked: a stream (`stream`), or a function or
  // an iterable as a PipelineStage; `chunk` is the type of the chunks before
  // it, and becomes that of those after it. `sinkValue` gets what the
  // Promise of a last function gives.
  Expr *streamStage(Expr *&arg, const Type *&chunk, bool first, bool last, bool &stream,
                    const Type **sinkValue);
  // Whether a stream that takes `writable` takes chunks of `chunk`.
  bool streamTakes(const Type *writable, const Type *chunk);
  // An iterator over the values of an iterable (the entries of a Map),
  // `xs.values()`; null for anything else.
  Expr *valuesIteratorOf(Expr *collection);

private:
  Checker &checker_;
};

} // namespace tz
