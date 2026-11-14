#pragma once

#include "compiler/semantic/code/comparisons/code_comparisons_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>

namespace tz {

class ComparisonRules {
public:
  explicit ComparisonRules(SemanticState &owner);

  void pushLiteralAdapting(BinaryExpr *binary);


  std::string mixedNumbersText(const BinaryExpr *binary) const;
  std::string operandTypesText(llvm::StringRef op, const Type *lhs, const Type *rhs) const;

private:
  struct NumberConversion {
    const Type *target = nullptr;
    bool isLeftConverted = false;
  };

  bool isShiftOperator(BinaryOp op) const;
  bool isBitwiseOperator(BinaryOp op) const;
  bool isNumberLike(const Type *type) const;


  std::string convertedText(llvm::StringRef operand, const std::string &method) const;
  std::string mixedExample(const BinaryExpr *binary, const NumberConversion &conversion) const;


  NumberConversion mixedConversion(const Type *leftType, const Type *rightType) const;

  SemanticState &state;
};

}
