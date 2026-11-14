#include "compiler/semantic/code/arithmetic/code_arithmetic_registry.h"

namespace tz {

CodeArithmeticRegistry::CodeArithmeticRegistry(SemanticState &owner)
    : arithmeticRules(owner),
      bitOperations(owner, this->arithmeticRules),
      constantTexts(owner),
      signOperations(owner, this->arithmeticRules),
      arithmeticOperators(owner, this->arithmeticRules) {}

void CodeArithmeticRegistry::constText(const ConstText &task) {
  return this->constantTexts.constText(task);
}

void CodeArithmeticRegistry::toInt32() {
  return this->arithmeticOperators.toInt32();
}

void CodeArithmeticRegistry::noteUnsignedVar(const NoteUnsignedVar &task) {
  return this->arithmeticOperators.noteUnsignedVar(task);
}

void CodeArithmeticRegistry::checkShift(const CheckShift &task) {
  return this->arithmeticOperators.checkShift(task);
}

void CodeArithmeticRegistry::checkArithmetic(const CheckArithmetic &task) {
  return this->arithmeticOperators.checkArithmetic(task);
}

void CodeArithmeticRegistry::afterArithmeticRight(const AfterArithmeticRight &task) {
  return this->arithmeticOperators.afterArithmeticRight(task);
}

void CodeArithmeticRegistry::afterArithmeticLeft(const AfterArithmeticLeft &task) {
  return this->arithmeticOperators.afterArithmeticLeft(task);
}

void CodeArithmeticRegistry::afterArithmeticOperands(const AfterArithmeticOperands &task) {
  return this->arithmeticOperators.afterArithmeticOperands(task);
}

void CodeArithmeticRegistry::finishArithmetic(const FinishArithmetic &task) {
  return this->arithmeticOperators.finishArithmetic(task);
}

void CodeArithmeticRegistry::resolveArithmetic(const ResolveArithmetic &task) {
  return this->arithmeticOperators.resolveArithmetic(task);
}

void CodeArithmeticRegistry::arithmeticResult(const ArithmeticResult &task) {
  return this->arithmeticOperators.arithmeticResult(task);
}

void CodeArithmeticRegistry::finishIntegerArithmetic(const FinishIntegerArithmetic &task) {
  return this->arithmeticOperators.finishIntegerArithmetic(task);
}

void CodeArithmeticRegistry::checkWrappingDivisor(const CheckWrappingDivisor &task) {
  return this->arithmeticOperators.checkWrappingDivisor(task);
}

void CodeArithmeticRegistry::startBits(const StartBits &task) {
  return this->arithmeticOperators.startBits(task);
}

void CodeArithmeticRegistry::finishBits(const FinishBits &task) {
  return this->bitOperations.finishBits(task);
}

void CodeArithmeticRegistry::foldBinaryNode(const FoldBinaryNode &task) {
  return this->arithmeticOperators.foldBinaryNode(task);
}

void CodeArithmeticRegistry::checkSign(const CheckSign &task) {
  return this->signOperations.checkSign(task);
}

void CodeArithmeticRegistry::finishSign(const FinishSign &task) {
  return this->signOperations.finishSign(task);
}

void CodeArithmeticRegistry::signOperandReady(const SignOperandReady &task) {
  return this->signOperations.signOperandReady(task);
}

void CodeArithmeticRegistry::checkBitNot(const CheckBitNot &task) {
  return this->bitOperations.checkBitNot(task);
}

void CodeArithmeticRegistry::finishBitNot(const FinishBitNot &task) {
  return this->bitOperations.finishBitNot(task);
}

}
