#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/stmt/loop_checker/loop_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class LoopChecker {
public:
  explicit LoopChecker(SemanticState &owner);

  void checkWhile(WhileStmt *stmt);
  void checkDoWhile(DoWhileStmt *stmt);
  void checkFor(ForStmt *stmt);
  void checkForOf(ForOfStmt *stmt);
  void checkLoopBody(Stmt *body, bool awaits);

private:
  static bool continuesTo(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels,
                          unsigned loops = 0);
  using ResultIterableType = Result<const Type *, LoopCheckerIterableTypeError>;
  using ResultBindingTyped = Result<const VarDeclarator *, LoopCheckerBindingTypeError>;
  using ResultBindingShape = Result<const Type *, LoopCheckerBindingShapeError>;

  ResultIterableType iterableType(const Type *type) const;
  static ResultBindingTyped bindingType(const ForOfStmt *stmt);
  static ResultBindingShape bindingShape(const ForOfStmt *stmt, const Type *element);

  SemanticState &state;
};

}
