#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/conversions/code_conversions_registry_errors.h"
#include "compiler/semantic/code/conversions/conversion_rules/conversion_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <utility>
#include <vector>

namespace tz {

class Coercions {
public:
  Coercions(SemanticState &owner, ConversionRules &sharedConversionRules);

  void coerce(const Coerce &task);
  void coerceSlot(const CoerceSlot &task);
  void coerceIteratorFound(const CoerceIteratorFound &task);
  void coerceAsyncIteratorFound(const CoerceAsyncIteratorFound &task);

private:
  using ResultRangeWasProven = Result<const Expr *, CodeConversionsRegistryRangeProvenError>;

  void replaceCoerced(Expr **slot, Expr *value);
  void coerceRange(Expr **slot, const Type *to, const std::string &what);
  void coerceFunction(Expr **slot, const Type *to, const std::string &what);
  void coerceAsyncIterator(Expr **slot, const Type *to, const std::string &what);


  bool isParamFitting(const Type *param, const Type *wanted) const;
  bool canProveSomeInterval(const Expr *expr,
                            llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals);


  Expr *coerced(Expr **slot);
  Expr *adaptResult(Expr *expr, const Type *to);
  Expr *adaptFunction(Expr *expr, const Type *to);


  const Type *functionTarget(const Type *to) const;
  FunctionDecl *namedFunction(const Expr *expr) const;
  std::vector<std::pair<int64_t, int64_t>> targetIntervals(const Type *to) const;
  std::pair<int64_t, int64_t>
  widestInterval(llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) const;
  std::string rangeCheck(const Expr *expr, const Type *target,
                         llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) const;
  ResultRangeWasProven rangeProven(const Expr *expr, const Type *to, llvm::StringRef what,
                                   llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) const;

  SemanticState &state;
  ConversionRules &conversionRules;
};

}
