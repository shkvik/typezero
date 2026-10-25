#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Truthiness, && and ||.
class TruthLowerer {
public:
  explicit TruthLowerer(CodeGen &codegen);

  llvm::Value *emitLogical(BinaryExpr *bin);
  // What `a && b` or `a || b` gives when a decides: a's value `lhs` (owned),
  // as the type of the whole.
  llvm::Value *decidedValue(BinaryExpr *bin, llvm::Value *lhs);
  // Whether a borrowed value is truthy, as a condition tests it (see Sema's
  // checkTruthy): a string when it is not empty, `T | undefined` when it
  // holds a value that is.
  llvm::Value *emitTruth(llvm::Value *value, const Type *type);
  // The truth of an expression converted to boolean.
  llvm::Value *emitTruthOf(Expr *expr);

private:
  CodeGen &codegen_;
};

} // namespace tz
