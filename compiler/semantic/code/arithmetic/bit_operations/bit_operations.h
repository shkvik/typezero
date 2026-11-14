#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/arithmetic/arithmetic_rules/arithmetic_rules.h"
#include "compiler/semantic/code/arithmetic/code_arithmetic_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <string>

namespace tz {

class BitOperations {
public:
  BitOperations(SemanticState &owner, ArithmeticRules &sharedArithmeticRules);

  void finishBits(const FinishBits &task);
  void checkBitNot(const CheckBitNot &task);
  void finishBitNot(const FinishBitNot &task);

private:
  using ResultOperandIsInteger = Result<const Type *, CodeArithmeticRegistryBitsOperandError>;

  bool isRoundingCall(const Expr *expr) const;


  std::string shownOperand(llvm::StringRef text) const;
  std::string doubleBitsText(const BinaryExpr *binary) const;
  std::string signedShiftText(const BinaryExpr *binary) const;
  std::string nothingChangesText(const BinaryExpr *binary) const;
  std::string nothingChangesHint(const BinaryExpr *binary, llvm::StringRef lhsText) const;
  std::string bitsProblemText(const BinaryExpr *binary,
                              CodeArithmeticRegistryCheckBitsErrorCode problem) const;


  ResultOperandIsInteger checkBitsOperand(const Type *type) const;
  std::optional<CodeArithmeticRegistryCheckBitsErrorCode>
  bitsProblem(const BinaryExpr *binary) const;

  SemanticState &state;
  ArithmeticRules &arithmeticRules;
};

}
