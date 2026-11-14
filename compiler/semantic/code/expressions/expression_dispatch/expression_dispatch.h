#pragma once

#include "compiler/semantic/code/expressions/code_expressions_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>

namespace tz {

class ExpressionDispatch {
public:
  explicit ExpressionDispatch(SemanticState &owner);

  void checkExpr(const CheckExpr &task);
  void reportError(const ReportError &task);
  void checkExprSlot(const CheckExprSlot &task);
  void checkExprKind(const CheckExprKind &task);
  void finishSequence(const FinishSequence &task);
  void writeCheckedSlot(const WriteCheckedSlot &task);
  void nextSequenceItem(const NextSequenceItem &task);
  void afterSequenceItem(const AfterSequenceItem &task);
  void finishOptionalChain(const FinishOptionalChain &task);

private:
  void startCast(CastExpr *castExpr);
  void startOptionalChain(OptionalChainExpr *chain);
  void checkNameValue(NameExpr *name, const Type *expected);
  void checkBoolLiteral(BoolLitExpr *literalExpr, const Type *expected);
  void checkFloatLiteral(FloatLitExpr *literalExpr, const Type *expected);
  void checkStringLiteral(StringLitExpr *literalExpr, const Type *expected);
  void startConditional(ConditionalExpr *conditional, const Type *expected);
  void checkFunctionLiteral(FunctionExpr *functionExpr, const Type *expected);


  bool isJsonNull(const NameExpr *name, const Type *expected) const;


  const Type *findForcedType(const Expr *expr) const;
  FuncSymbol *findGenericFunction(const Expr *expr) const;


  Signature signatureOf(const Type *functionType) const;
  std::optional<Signature> expectedSignature(const Type *expected) const;
  EnumDecl *findEnumObject(const NameExpr *name, const Type *expected) const;

  SemanticState &state;
};

}
