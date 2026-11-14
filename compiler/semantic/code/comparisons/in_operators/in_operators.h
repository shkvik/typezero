#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/comparisons/code_comparisons_registry_errors.h"
#include "compiler/semantic/code/comparisons/comparison_rules/comparison_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

class InOperators {
public:
  InOperators(SemanticState &owner, ComparisonRules &sharedComparisonRules);

  void checkInOperator(const CheckInOperator &task);
  void afterInOperands(const AfterInOperands &task);

private:
  using ResultInOperatorSuited = Result<Expr *, CodeComparisonsRegistryCheckInError>;

  bool isIdentifierChar(char letter) const;


  std::string fieldText(const Expr *object, llvm::StringRef field) const;
  llvm::SMLoc inErrorPlace(const BinaryExpr *binary,
                           CodeComparisonsRegistryCheckInErrorCode code) const;
  std::string inBoundsText(const std::string &indexText, const std::string &array,
                           bool isLiteralKey) const;


  ResultInOperatorSuited checkObjectIn(BinaryExpr *binary, const Type *object);
  ResultInOperatorSuited checkUnionIn(BinaryExpr *binary, const Type *rightType);
  ResultInOperatorSuited checkFieldRequired(BinaryExpr *binary, const Type *member,
                                            const std::string &name) const;
  ResultInOperatorSuited checkUnionKeyIn(BinaryExpr *binary, const Type *options,
                                         const Type *objects, const std::string &name) const;
  ResultInOperatorSuited checkInTarget(BinaryExpr *binary, const std::optional<std::string> &key,
                                       bool isLiteralKey, const std::string &array);


  std::optional<std::string> chainText(const Expr *expr) const;
  std::optional<std::string> simpleText(const Expr *expr) const;


  std::vector<const Type *> objectMembers(const Type *objects) const;
  size_t countWithField(llvm::ArrayRef<const Type *> members, const std::string &name) const;

  SemanticState &state;
  ComparisonRules &comparisonRules;
};

}
