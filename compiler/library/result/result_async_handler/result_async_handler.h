#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The functions that run the methods of ResultAsync when its promise settles.
class ResultAsyncHandler {
public:
  explicit ResultAsyncHandler(Lowering &lowering);

  // ResultAsync: a Promise of a Result, and the reactions its functions and
  // methods make.
  llvm::Function *resultAsyncHandler(Builtin kind, const Type *value, const Type *result,
                                     const Type *first, const Type *second);

private:
  Lowering &lowering_;
};

} // namespace tz
