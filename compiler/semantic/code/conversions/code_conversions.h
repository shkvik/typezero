#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>

namespace tz {

struct CodeStorage;

class CodeConversions {
public:
  explicit CodeConversions(CodeStorage &owner);

  bool isNumberLike(const Type *type) const;
  bool isIntLiteralTree(const Expr *expr) const;
  bool isNumberLiteralTree(const Expr *expr) const;
  bool isLiteralAdapting(const Expr *expr, const Type *to) const;


  std::string operandText(llvm::StringRef text) const;
  std::string conversionMethod(const Type *to) const;
  std::string conversionHint(const Expr *expr, const Type *to) const;


  const Type *literalHint(const Expr *other, const Type *hint) const;
  const Type *literalTarget(const Expr *expr, const Type *to) const;

private:
  bool isNameUnit(char unit) const;
  bool isPlainOperand(llvm::StringRef text) const;
  bool isArithmeticOperator(BinaryOp op) const;
  bool isIntLiteralNode(const Expr *expr) const;
  bool isNumberLiteralNode(const Expr *expr) const;
  bool isWrappedInParens(llvm::StringRef text) const;
  bool isValueFitting(int64_t value, const Type *from, const Type *target) const;


  std::string shownOperand(const Expr *expr) const;
  std::string shownMember(const MemberExpr *member) const;


  const Type *numberTarget(const Type *to) const;
  const Type *wholeNumberMember(const Type *type) const;

  CodeStorage &code;
};

}
