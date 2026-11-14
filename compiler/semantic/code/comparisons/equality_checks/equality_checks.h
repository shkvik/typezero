#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/comparisons/code_comparisons_registry_errors.h"
#include "compiler/semantic/code/comparisons/comparison_rules/comparison_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class EqualityChecks {
public:
  EqualityChecks(SemanticState &owner, ComparisonRules &sharedComparisonRules);

  void checkEquality(const CheckEquality &task);
  void walkTypeOfName(const WalkTypeOfName &task);
  void finishEqualOperands(const FinishEqualOperands &task);
  void afterEqualityOperands(const AfterEqualityOperands &task);

private:
  using ResultNoResultOperand = Result<const BinaryExpr *, CodeComparisonsRegistryNoResultsError>;
  using ResultTypeOfNameKnown =
      Result<const StringLitExpr *, CodeComparisonsRegistryCheckTypeOfNameError>;

  void reportIncomparable(BinaryExpr *binary);
  void walkTypeOfSide(const Expr *side, const Expr *other);


  bool isNullName(const Expr *expr) const;


  std::string noNumberText(const Expr *operand) const;
  std::string numberNamesHere(const Expr *operand) const;
  std::string quotedNames(llvm::ArrayRef<llvm::StringRef> names) const;


  ResultNoResultOperand checkNoResults(const BinaryExpr *binary) const;
  ResultTypeOfNameKnown checkTypeOfName(const Expr *operand, const StringLitExpr *text);
  ResultTypeOfNameKnown checkKnownTypeOfName(const Expr *operand, const StringLitExpr *text,
                                             const std::string &name) const;
  ResultTypeOfNameKnown checkTypeOfNameMatches(const Expr *operand, const StringLitExpr *text,
                                               const std::string &name) const;


  const char *nameSeparator(size_t index, size_t count) const;
  std::vector<llvm::StringRef> numberTypeOfNames(const Type *type) const;

  SemanticState &state;
  ComparisonRules &comparisonRules;
};

}
