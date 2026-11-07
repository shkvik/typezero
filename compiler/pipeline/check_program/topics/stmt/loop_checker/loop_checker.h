#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/stmt/loop_checker/loop_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class LoopChecker {
public:
  explicit LoopChecker(CheckState &owner);

  void checkWhile(WhileStmt *stmt);
  void checkDoWhile(DoWhileStmt *stmt);
  void checkFor(ForStmt *stmt);
  void checkForOf(ForOfStmt *stmt);
  void checkLoopBody(Stmt *body, bool awaits);

private:
  using ResultIterableType = Result<const Type *, LoopCheckerIterableTypeError>;
  using ResultBindingTyped = Result<const VarDeclarator *, LoopCheckerBindingTypeError>;
  using ResultBindingShape = Result<const Type *, LoopCheckerBindingShapeError>;

  ResultIterableType iterableType(const Type *type) const;
  static ResultBindingTyped bindingType(const ForOfStmt *stmt);
  static ResultBindingShape bindingShape(const ForOfStmt *stmt, const Type *element);

  CheckState &state;
};

}
