#pragma once

#include "compiler/semantic/code/arithmetic/arithmetic_operators/arithmetic_operators.h"
#include "compiler/semantic/code/arithmetic/arithmetic_rules/arithmetic_rules.h"
#include "compiler/semantic/code/arithmetic/bit_operations/bit_operations.h"
#include "compiler/semantic/code/arithmetic/constant_texts/constant_texts.h"
#include "compiler/semantic/code/arithmetic/sign_operations/sign_operations.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeArithmeticRegistry {
public:
  explicit CodeArithmeticRegistry(SemanticState &owner);

  void toInt32();
  void constText(const ConstText &task);
  void startBits(const StartBits &task);
  void checkSign(const CheckSign &task);
  void checkShift(const CheckShift &task);
  void finishBits(const FinishBits &task);
  void finishSign(const FinishSign &task);
  void checkBitNot(const CheckBitNot &task);
  void finishBitNot(const FinishBitNot &task);
  void foldBinaryNode(const FoldBinaryNode &task);
  void noteUnsignedVar(const NoteUnsignedVar &task);
  void checkArithmetic(const CheckArithmetic &task);
  void finishArithmetic(const FinishArithmetic &task);
  void arithmeticResult(const ArithmeticResult &task);
  void signOperandReady(const SignOperandReady &task);
  void resolveArithmetic(const ResolveArithmetic &task);
  void afterArithmeticLeft(const AfterArithmeticLeft &task);
  void afterArithmeticRight(const AfterArithmeticRight &task);
  void checkWrappingDivisor(const CheckWrappingDivisor &task);
  void afterArithmeticOperands(const AfterArithmeticOperands &task);
  void finishIntegerArithmetic(const FinishIntegerArithmetic &task);

private:
  ArithmeticRules arithmeticRules;
  BitOperations bitOperations;
  ConstantTexts constantTexts;
  SignOperations signOperations;
  ArithmeticOperators arithmeticOperators;
};

}
