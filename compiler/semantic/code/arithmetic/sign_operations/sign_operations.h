#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/arithmetic/arithmetic_rules/arithmetic_rules.h"
#include "compiler/semantic/code/arithmetic/code_arithmetic_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class SignOperations {
public:
  SignOperations(SemanticState &owner, ArithmeticRules &sharedArithmeticRules);

  void checkSign(const CheckSign &task);
  void finishSign(const FinishSign &task);
  void signOperandReady(const SignOperandReady &task);

private:
  using ResultNumberForTheSign = Result<const Type *, CodeArithmeticRegistrySignOperandError>;
  using ResultOperandIsNumeric = Result<const Type *, CodeArithmeticRegistryNumberOperandError>;

  ResultNumberForTheSign checkSignOperand(llvm::StringRef op, const Type *type) const;
  ResultOperandIsNumeric checkNumberOperand(const UnaryExpr *unary, llvm::StringRef op,
                                            const Type *type, bool isLiteral) const;


  const Type *signHint(const UnaryExpr *unary, const Type *expected) const;

  SemanticState &state;
  ArithmeticRules &arithmeticRules;
};

}
