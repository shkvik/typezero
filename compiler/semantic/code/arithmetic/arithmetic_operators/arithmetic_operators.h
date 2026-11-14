#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/arithmetic/arithmetic_rules/arithmetic_rules.h"
#include "compiler/semantic/code/arithmetic/code_arithmetic_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class ArithmeticOperators {
public:
  ArithmeticOperators(SemanticState &owner, ArithmeticRules &sharedArithmeticRules);

  void toInt32();
  void startBits(const StartBits &task);
  void checkShift(const CheckShift &task);
  void foldBinaryNode(const FoldBinaryNode &task);
  void noteUnsignedVar(const NoteUnsignedVar &task);
  void checkArithmetic(const CheckArithmetic &task);
  void finishArithmetic(const FinishArithmetic &task);
  void arithmeticResult(const ArithmeticResult &task);
  void resolveArithmetic(const ResolveArithmetic &task);
  void afterArithmeticLeft(const AfterArithmeticLeft &task);
  void afterArithmeticRight(const AfterArithmeticRight &task);
  void checkWrappingDivisor(const CheckWrappingDivisor &task);
  void afterArithmeticOperands(const AfterArithmeticOperands &task);
  void finishIntegerArithmetic(const FinishIntegerArithmetic &task);

private:
  struct OperandHints {
    const Type *hint = nullptr;
    const Type *rhsHint = nullptr;
    bool isBitwise = false;
    bool canHintLiterals = false;
  };

  using ResultDivisorNotZero = Result<const Expr *, CodeArithmeticRegistryCheckDivisorError>;
  using ResultExponentIsValid = Result<const Expr *, CodeArithmeticRegistryPowerExponentError>;

  void pushLiteralAdapting(BinaryExpr *binary);
  void startNumberArithmetic(BinaryExpr *binary);


  bool isBitwiseOperator(BinaryOp op) const;
  bool isNumberLike(const Type *type) const;
  bool isIntLiteralNode(const Expr *expr) const;
  bool hasOnlyIntLiterals(const Expr *expr) const;


  ResultDivisorNotZero checkDivisor(BinaryOp op, const Expr *divisor) const;
  OperandHints operandHints(const BinaryExpr *binary, const Type *expected) const;
  ResultExponentIsValid checkPowerExponent(const Expr *rhs, const Type *common) const;
  const Type *integerHint(const Expr *literal, const Expr *other, const Type *hint,
                          bool isBitwise) const;

  SemanticState &state;
  ArithmeticRules &arithmeticRules;
};

}
