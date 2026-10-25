#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Generators inlined into the loop that consumes them.
class InlineGeneratorLowerer {
public:
  explicit InlineGeneratorLowerer(Lowering &lowering);

  // Evaluates the operands of an iteration of `source`, in the order
  // JavaScript does, into slots of the current scope.
  Lowering::Producer prepare(Expr *source, llvm::Value *returnSlot = nullptr);
  bool isInlinable(const FunctionDecl *decl) const;
  // The body of `decl`, whose parameters are bound, with `consumer` at each
  // `yield`; what it returns goes to `returnSlot`, if anywhere.
  void emitInlineGenerator(FunctionDecl *decl, const Lowering::Consumer &consumer,
                           llvm::Value *returnSlot);
  llvm::Value *emitYield(YieldExpr *yield);
  // Gives `value` to whoever iterates the generator being emitted; the
  // result is what comes back, as `resultType`.
  llvm::Value *emitYieldValue(llvm::Value *value, const Type *resultType);

private:
  Lowering &lowering_;
  // How many times the code being emitted is copied by the generators
  // emitted inline around it, one copy for each `yield`.
  unsigned inlineCopies_ = 1;
};

} // namespace tz
