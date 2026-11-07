#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/stream/stream_pipeline/stream_pipeline_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class StreamPipeline {
public:
  explicit StreamPipeline(CheckState &owner);

  Expr *checkStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc);
  Expr *checkLibraryCall(CallExpr *call, NameExpr *name, FuncSymbol *func);
  Expr *streamStage(Expr *&arg, const Type *&chunk, bool first, bool last, bool &stream,
                    const Type **sinkValue);
  bool streamTakes(const Type *writable, const Type *chunk);
  Expr *valuesIteratorOf(Expr *collection);

private:
  using ResultWritableEnd = Result<const Type *, StreamPipelineWritableEndError>;
  using ResultReadableEnd = Result<const Type *, StreamPipelineReadableEndError>;

  static ResultWritableEnd writableEnd(const StreamChunks &chunks, const Type *type);
  static ResultReadableEnd readableEnd(const StreamChunks &chunks, const Type *type);
  static bool inModule(const std::string &place, llvm::StringRef module);

  CheckState &state;
};

}
