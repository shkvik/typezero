#include "compiler/semantic/declare/assigned_fields/assigned_fields.h"
#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

AssignedFields::AssignedFields(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->keyedFieldChecker = &all.keyedFieldChecker;
}

void AssignedFields::rememberAssigned(SourceFile &program) {
  PlacedNodes nodes = PlacedNodes::collect(program.items, {});
  for (const PlacedExpr &placed : nodes.getExprs()) {
    this->rememberTarget(placed.expr);
  }
}

void AssignedFields::rememberTarget(const Expr *expr) {
  const Expr *target = nullptr;
  if (auto *assign = dyn_cast<AssignExpr>(expr)) {
    target = assign->target;
  } else if (auto *update = dyn_cast<UpdateExpr>(expr)) {
    target = update->target;
  }
  auto markAssigned = [&](llvm::StringRef name) {
    this->state.getFlowState().markFieldAssigned(name);
  };
  if (auto *member = dyn_cast_or_null<MemberExpr>(target)) {
    markAssigned(member->name);
  } else if (auto *index = dyn_cast_or_null<IndexExpr>(target)) {
    this->keyedFieldChecker->keyNames(index->index, markAssigned);
  }
}

}
