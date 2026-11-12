#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/literal_checker/literal_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ExprChecker;
class LiteralTypes;
class ConcatChecker;
class TypeOperators;

enum class LiteralSign {
  PLAIN,
  NEGATED,
};

class LiteralChecker {
public:
  using ResultIntLiteralTyped = Result<Expr *, LiteralCheckerCheckIntLitError>;

  LiteralChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkTemplate(TemplateExpr *templateExpr, const Type *expected = nullptr);
  ResultIntLiteralTyped checkIntLit(IntLitExpr *literalExpr, const Type *expected,
                                    LiteralSign sign = LiteralSign::PLAIN);

private:
  bool isFitting(const IntLitExpr *literal, const Type *type, LiteralSign sign) const;
  const Type *literalTypeFor(const IntLitExpr *literalExpr, const Type *expected,
                             LiteralSign sign) const;
  const Type *numberTarget(const Type *expected) const;
  void rememberForcedType(IntLitExpr *literalExpr);
  bool isInsideGeneric() const;
  const Expr *templatePart(const Expr *expr) const;
  bool isConstantPart(const Expr *part) const;
  bool isTemplated(const Type *candidate) const;
  bool isWantingTemplate(const Type *want) const;
  bool isTemplatePartType(const Type *part) const;
  std::optional<std::vector<const Type *>>
  templateOperands(const TemplateExpr *templateExpr, llvm::ArrayRef<const Type *> parts) const;
  Expr *typedTemplate(TemplateExpr *templateExpr, const std::vector<const Type *> &operands,
                      const Type *want, bool isConstant);

  SemanticState &state;
  ExprChecker *exprChecker = nullptr;
  LiteralTypes *literalTypes = nullptr;
  ConcatChecker *concatChecker = nullptr;
  TypeOperators *typeOperators = nullptr;
};

}
