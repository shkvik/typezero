#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

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
  static bool inModule(const std::string &place, llvm::StringRef module);

  CheckState &state;
};

}
