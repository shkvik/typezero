#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The functions that run the methods of ResultAsync when its promise settles.
class ResultAsyncHandler {
public:
  explicit ResultAsyncHandler(CodeGen &codegen);

  // ResultAsync: a Promise of a Result, and the reactions its functions and
  // methods make.
  llvm::Function *resultAsyncHandler(Builtin kind, const Type *value, const Type *result,
                                     const Type *first, const Type *second);

private:
  CodeGen &codegen_;
};

} // namespace tz
