#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/literals/code_literals_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <vector>

namespace tz {

class ScalarLiterals {
public:
  explicit ScalarLiterals(SemanticState &owner);

  void checkTemplate(const CheckTemplate &task);
  void checkIntLiteral(const CheckIntLiteral &task);
  void nextTemplatePart(const NextTemplatePart &task);
  void afterTemplatePart(const AfterTemplatePart &task);
  void afterTemplateType(const AfterTemplateType &task);

private:
  using ResultIntLiteralTyped = Result<Expr *, CodeLiteralsRegistryCheckIntLitError>;

  void rememberForcedType(IntLitExpr *literalExpr);
  void finishTemplateParts(const TemplateDraft &draft);


  bool isInsideGeneric() const;
  bool isConstantPart(const Expr *part) const;
  bool isTemplated(const Type *candidate) const;
  bool isWantingTemplate(const Type *want) const;
  bool isTemplatePartType(const Type *part) const;
  bool isFitting(const IntLitExpr *literal, const Type *type, LiteralSign sign) const;


  const Expr *templatePart(const Expr *expr) const;
  const Type *numberTarget(const Type *expected) const;
  const Type *literalTypeFor(const IntLitExpr *literalExpr, const Type *expected,
                             LiteralSign sign) const;


  ResultIntLiteralTyped checkIntLit(IntLitExpr *literalExpr, const Type *expected,
                                    LiteralSign sign);
  std::optional<std::vector<const Type *>>
  templateOperands(const TemplateExpr *templateExpr, llvm::ArrayRef<const Type *> parts) const;

  SemanticState &state;
};

}
