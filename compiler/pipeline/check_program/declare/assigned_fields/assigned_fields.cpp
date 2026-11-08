#include "compiler/pipeline/check_program/declare/assigned_fields/assigned_fields.h"
#include "compiler/pipeline/check_program/ast_walker/ast_walker.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/objects/keyed_field_checker/keyed_field_checker.h"

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

AssignedFields::AssignedFields(CheckState &owner)
    : state(owner) {}

void AssignedFields::rememberAssigned(SourceFile &program) {
  auto rememberTarget = [&](const Expr *expr) { this->rememberTarget(expr); };
  for (Stmt *item : program.items) {
    AstWalker::visitExprs(item, rememberTarget);
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
    KeyedFieldChecker(this->state).keyNames(index->index, markAssigned);
  }
}

}
