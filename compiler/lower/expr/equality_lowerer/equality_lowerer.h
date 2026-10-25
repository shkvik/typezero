#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// ==, != and ??.
class EqualityLowerer {
public:
  explicit EqualityLowerer(CodeGen &codegen);

  llvm::Value *emitEquality(BinaryExpr *bin);
  llvm::Value *emitNullish(BinaryExpr *bin);
  // Whether two values of `type` are equal (==); both borrowed.
  llvm::Value *emitValuesEqual(llvm::Value *a, llvm::Value *b, const Type *type);

private:
  CodeGen &codegen_;
};

} // namespace tz
