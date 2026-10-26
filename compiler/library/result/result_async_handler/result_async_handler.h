#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ResultAsyncHandler {
public:
  explicit ResultAsyncHandler(Lowering &lowering);

  llvm::Function *resultAsyncHandler(Builtin kind, const Type *value, const Type *result,
                                     const Type *first, const Type *second);

private:
  Lowering &lowering_;
};

}
