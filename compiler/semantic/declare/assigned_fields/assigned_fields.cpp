#include "compiler/semantic/declare/assigned_fields/assigned_fields.h"
#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

using llvm::dyn_cast;

namespace tz {

AssignedFields::AssignedFields(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->keyedFieldChecker = &all.keyedFieldChecker;
}

void AssignedFields::rememberAssigned(SourceFile &program) {
  auto nodes = PlacedNodes::collect(program.items, {});
  for (const PlacedExpr &placed : nodes.getExprs()) {
    this->rememberTarget(placed.expr);
  }
}

void AssignedFields::rememberTarget(const Expr *expr) {
  const Expr *target = this->changedTarget(expr);
  if (target == nullptr) {
    return;
  }
  auto *member = dyn_cast<MemberExpr>(target);
  auto *index = dyn_cast<IndexExpr>(target);
  if (member != nullptr) {
    this->state.getFlowState().markFieldAssigned(member->name);
  }
  if (index != nullptr) {
    for (const std::string &name : this->keyedFieldChecker->keyNames(index->index)) {
      this->state.getFlowState().markFieldAssigned(name);
    }
  }
}

const Expr *AssignedFields::changedTarget(const Expr *expr) const {
  auto *assign = dyn_cast<AssignExpr>(expr);
  auto *update = dyn_cast<UpdateExpr>(expr);
  if (assign != nullptr) {
    return assign->target;
  }
  if (update != nullptr) {
    return update->target;
  }

  return nullptr;
}

}
