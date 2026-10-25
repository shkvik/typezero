#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Result, its methods and yield*.
class ResultLowerer {
public:
  explicit ResultLowerer(Lowering &lowering);

  Lowering::ResultLayout resultLayout(const Type *type);
  llvm::Type *lowerHalf(const Type *half);
  llvm::Constant *resultDescriptor(const Type *type);
  llvm::Value *resultIsOk(llvm::Value *result, const Type *type);
  // The value (or with `error`, the error) of a Result, as it lies there.
  llvm::Value *resultHalf(llvm::Value *result, const Type *type, bool error);
  // A Result of `type` from its halves; null for zero.
  llvm::Value *makeResult(const Type *type, llvm::Value *ok, llvm::Value *value,
                          llvm::Value *error);
  // An owned half of type `from` as a half of type `to`; null for zero when
  // it cannot hold anything.
  llvm::Value *convertHalf(llvm::Value *half, const Type *from, const Type *to);
  // Calls the callback `fn` with the owned half `half`, if it takes one.
  llvm::Value *callWithHalf(llvm::Value *fn, const Type *fnType, llvm::Value *half,
                            const Type *halfType);
  llvm::Value *emitResultCall(CallExpr *call);
  llvm::Value *emitResultMethod(CallExpr *call);
  llvm::Value *emitResultMember(MemberExpr *member);
  // `yield* r`: an Err returns from the generator of safeTry.
  llvm::Value *emitSafeTryYield(YieldExpr *yield);

private:
  Lowering &lowering_;
  llvm::DenseMap<const Type *, Lowering::ResultLayout> resultLayouts_;
};

} // namespace tz
