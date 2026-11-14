#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/flow_v2/proofs/flow_proofs_errors.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include "llvm/ADT/ArrayRef.h"

namespace tz {

class SemanticState;

class FlowProofs {
public:
  using ResultValueFitsType = Result<const Expr *, FlowProofsValueFitsError>;
  using ResultLessWasProven = Result<const Expr *, FlowProofsRequireLessError>;
  using ResultUpToWasProven = Result<const Expr *, FlowProofsRequireUpToError>;
  using ResultBetweenProven = Result<const Expr *, FlowProofsRequireBetweenError>;

  explicit FlowProofs(SemanticState &owner);

  bool canProveNotEmpty(const Expr *object) const;
  bool canProveInBounds(const Expr *object, const Expr *index) const;
  bool isExempt(llvm::SMLoc loc, const Expr *object = nullptr) const;
  bool canProveAtInBounds(const Expr *object, const Expr *index) const;
  bool canProveFits(const Expr *expr, int64_t low, int64_t high) const;
  bool canProveInBoundsOrEnd(const Expr *object, const Expr *index) const;
  bool canProveSumUpTo(std::optional<Linear> first, std::optional<Linear> second,
                       const Expr *object) const;


  ResultLessWasProven requireLess(const Expr *left, const Expr *right, llvm::StringRef what);
  ResultValueFitsType checkValueFits(const Expr *value, const Type *target, llvm::StringRef what);
  ResultUpToWasProven requireUpTo(const Expr *value, const Expr *object, int64_t less,
                                  llvm::StringRef what);
  ResultBetweenProven requireBetween(const Expr *expr, int64_t low, std::optional<int64_t> high,
                                     llvm::StringRef what);

private:
  bool isNeverType(const Type *type) const;
  bool isNeverObject(const Expr *object) const;
  bool canProveNonNegative(const Expr *expr) const;
  bool isValueWidening(const CastExpr *conversion) const;
  bool canProveLess(const Expr *left, const Expr *right) const;
  bool isLinearAtLeast(const std::optional<Linear> &value, int64_t low) const;
  bool isLinearAtMost(const std::optional<Linear> &value, int64_t high) const;
  bool canProveUpTo(const Expr *value, const Expr *object, int64_t less) const;
  bool canProveLinearUpTo(Linear value, const Expr *object, int64_t less) const;
  bool canProveBetween(const Expr *expr, int64_t low, std::optional<int64_t> high) const;
  bool canProveIndexWithin(const Expr *object, const Expr *index, int64_t endExtra) const;
  bool canProveLinearIndex(const Linear &at, const Linear &length, int64_t endExtra) const;
  bool canProveValueBetween(const Expr *expr, int64_t low, std::optional<int64_t> high) const;


  std::optional<Linear> viewBufferLength(const Expr *object) const;
  std::optional<Linear> lengthOrBufferLength(const Expr *object) const;


  int64_t wrapped(Type::Bytes kind, int64_t v) const;
  const Expr *withoutWidening(const Expr *expr) const;
  const MemberExpr *viewBufferMember(const Expr *object) const;
  std::optional<std::string> shownValue(const Expr *value) const;
  std::vector<const Expr *> provedBranches(const Expr *expr) const;
  std::optional<Term> sumTerm(const Term &first, const Term &second) const;

  SemanticState &state;
};

}
