#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Which code an expression becomes; optional chains, conditionals and conversions.
class ExprLowerer {
public:
  explicit ExprLowerer(CodeGen &codegen);

  // Expressions
  llvm::Value *emitExpr(Expr *expr);
  llvm::Value *emitExprKind(Expr *expr);
  llvm::Value *emitOptionalChain(OptionalChainExpr *chain);
  llvm::Value *emitChainLink(CastExpr *link);
  llvm::Value *emitConditional(ConditionalExpr *cond);
  // Converts an owned value; the result is owned.
  llvm::Value *emitConversion(llvm::Value *value, const Type *from, const Type *to);

private:
  CodeGen &codegen_;
};

} // namespace tz
