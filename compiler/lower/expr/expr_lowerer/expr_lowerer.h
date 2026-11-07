#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ExprLowerer {
public:
  explicit ExprLowerer(Lowering &owner);

  llvm::Value *emitExpr(Expr *expr);
  std::vector<llvm::Value *> emitExprs(llvm::ArrayRef<Expr *> exprs);
  llvm::Value *emitExprKind(Expr *expr);
  llvm::Value *emitOptionalChain(OptionalChainExpr *chain);
  llvm::Value *emitChainLink(CastExpr *link);
  llvm::Value *emitConditional(ConditionalExpr *cond);
  llvm::Value *emitConversion(llvm::Value *value, const Type *from, const Type *to);

private:
  static bool isReference(const Type *type);

  Lowering &lowering;
};

}
