#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Strings: literals, concatenation, templates, comparisons and indices.
class StringLowerer {
public:
  explicit StringLowerer(CodeGen &codegen);

  llvm::Value *stringObj(llvm::Value *str);
  llvm::Value *stringMeta(llvm::Value *str);
  llvm::Value *stringLength(llvm::Value *str); // as i64
  // Calls a runtime function that returns a string through its first argument.
  llvm::Value *callReturningString(llvm::FunctionCallee fn, llvm::ArrayRef<llvm::Value *> args);
  llvm::Value *concatOperands(llvm::ArrayRef<CodeGen::Operand> operands);
  CodeGen::Operand emitConcatPart(Expr *expr, bool canBorrow);
  llvm::Value *emitConcat(BinaryExpr *bin);
  llvm::Value *emitTemplate(TemplateExpr *tmpl);
  llvm::Value *emitStringComparison(BinaryExpr *bin);
  llvm::Value *emitStringIndex(IndexExpr *index);
  llvm::Constant *stringLiteral(const std::u16string &text);
  llvm::Constant *utf8Constant(llvm::StringRef text);

private:
  CodeGen &codegen_;
  llvm::StringMap<llvm::Constant *> utf8Constants_;
  std::map<std::u16string, llvm::Constant *> literals_;
};

} // namespace tz
