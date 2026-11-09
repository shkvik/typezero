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

class LiteralChecker {
public:
  using ResultIntLiteralTyped = Result<Expr *, LiteralCheckerCheckIntLitError>;

  LiteralChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkTemplate(TemplateExpr *tmpl, const Type *expected = nullptr);
  ResultIntLiteralTyped checkIntLit(IntLitExpr *lit, const Type *expected, bool negated);

private:
  static bool fitsIn(const IntLitExpr *lit, const Type *type, bool negated);

  SemanticState &state;
  ExprChecker *exprChecker = nullptr;
  LiteralTypes *literalTypes = nullptr;
  ConcatChecker *concatChecker = nullptr;
  TypeOperators *typeOperators = nullptr;
};

}
