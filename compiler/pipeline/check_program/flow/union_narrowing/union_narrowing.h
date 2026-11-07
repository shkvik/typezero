#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class UnionNarrowing {
public:
  explicit UnionNarrowing(CheckState &owner);

  static void narrowVariable(Flow &flow, const VarSymbol *var, const Type *type);
  std::optional<NarrowTarget> narrowTarget(const Expr *expr) const;
  const Type *targetType(const Flow &flow, const NarrowTarget &target) const;
  void narrowTargetTo(Flow &flow, const NarrowTarget &target, const Type *type) const;
  void addUnionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;

private:
  static llvm::ArrayRef<const Type *> membersOf(const Type *&type);
  static const VarSymbol *readVariable(const Expr *expr);
  static const Type *keepMembers(const Type *type, llvm::function_ref<bool(const Type *)> keep);
  CheckState &state;
};

}
