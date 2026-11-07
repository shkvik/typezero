#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/stmt/jump_checker/jump_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class JumpChecker {
public:
  using ResultJumpTargeted = Result<Stmt *, JumpCheckerCheckJumpError>;

  explicit JumpChecker(CheckState &owner);

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

  CheckState &state;
};

}
