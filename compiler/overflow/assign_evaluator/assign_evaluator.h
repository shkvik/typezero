#pragma once

#include "compiler/overflow/overflow_check/overflow_check.h"

namespace tz {

// Where values go: places, reads and writes of them, assignments, updates and object
// literals.
class AssignEvaluator {
public:
  explicit AssignEvaluator(OverflowCheck &check);

  Val evalAssign(const AssignExpr *assign, State &st);
  Val evalUpdate(const UpdateExpr *update, State &st);
  Val evalObjectLit(const ObjectLitExpr *lit, State &st);
  OverflowCheck::Place place(const Expr *target, State &st);
  Val read(const OverflowCheck::Place &place, State &st, const Expr *target);
  void write(const OverflowCheck::Place &place, const Val &v, State &st);

private:
  OverflowCheck &check_;
};

} // namespace tz
