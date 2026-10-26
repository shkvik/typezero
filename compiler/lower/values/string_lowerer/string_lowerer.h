#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class StringLowerer {
public:
  explicit StringLowerer(Lowering &lowering);

  llvm::Value *stringObj(llvm::Value *str);
  llvm::Value *stringMeta(llvm::Value *str);
  llvm::Value *stringLength(llvm::Value *str);
  llvm::Value *callReturningString(llvm::FunctionCallee fn, llvm::ArrayRef<llvm::Value *> args);
  llvm::Value *concatOperands(llvm::ArrayRef<Lowering::Operand> operands);
  Lowering::Operand emitConcatPart(Expr *expr, bool canBorrow);
  llvm::Value *emitConcat(BinaryExpr *bin);
  llvm::Value *emitTemplate(TemplateExpr *tmpl);
  llvm::Value *emitStringComparison(BinaryExpr *bin);
  llvm::Value *emitStringIndex(IndexExpr *index);
  llvm::Constant *stringLiteral(const std::u16string &text);
  llvm::Constant *utf8Constant(llvm::StringRef text);

private:
  Lowering &lowering_;
  llvm::StringMap<llvm::Constant *> utf8Constants_;
  std::map<std::u16string, llvm::Constant *> literals_;
};

}
