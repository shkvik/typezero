#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class InlineGeneratorLowerer {
public:
  explicit InlineGeneratorLowerer(Lowering &lowering);

  Lowering::Producer prepare(Expr *source, llvm::Value *returnSlot = nullptr);
  bool isInlinable(const FunctionDecl *decl) const;
  void emitInlineGenerator(FunctionDecl *decl, const Lowering::Consumer &consumer,
                           llvm::Value *returnSlot);
  llvm::Value *emitYield(YieldExpr *yield);
  llvm::Value *emitYieldValue(llvm::Value *value, const Type *resultType);

private:
  Lowering &lowering_;
  unsigned inlineCopies_ = 1;
};

}
