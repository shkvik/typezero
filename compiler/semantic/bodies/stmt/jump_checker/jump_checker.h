#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/stmt/jump_checker/jump_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class JumpChecker {
public:
  using ResultJumpTargeted = Result<Stmt *, JumpCheckerCheckJumpError>;

  explicit JumpChecker(SemanticState &owner);

  void checkLabeled(LabeledStmt *stmt);
  ResultJumpTargeted checkJump(Stmt *stmt);
  void pushTarget(JumpTarget::Kind kind);

private:
  using ResultLabelIsFresh = Result<const LabeledStmt *, JumpCheckerLabelFreshError>;
  using ResultBodyCanLabel = Result<const Stmt *, JumpCheckerLabelBodyError>;

  static bool isLoop(const Stmt *stmt);
  static ResultBodyCanLabel labelBody(const Stmt *body);
  ResultLabelIsFresh labelFresh(llvm::ArrayRef<llvm::StringRef> labels,
                                const LabeledStmt *labeled) const;

  SemanticState &state;
};

}
