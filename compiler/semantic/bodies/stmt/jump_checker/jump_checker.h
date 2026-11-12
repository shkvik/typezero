#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/stmt/jump_checker/jump_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class FlowChecker;
class SemanticParts;
class StmtChecker;

class JumpChecker {
public:
  using ResultJumpTargeted = Result<Stmt *, JumpCheckerCheckJumpError>;

  JumpChecker(SemanticState &owner, SemanticParts &all);

  void walkLabeled(LabeledStmt *stmt);
  ResultJumpTargeted checkJump(Stmt *stmt);
  void pushTarget(JumpTarget::Kind kind);

private:
  using ResultLabelIsFresh = Result<const LabeledStmt *, JumpCheckerLabelFreshError>;
  using ResultBodyCanLabel = Result<const Stmt *, JumpCheckerLabelBodyError>;
  using ResultTargetFound = Result<const JumpTarget *, JumpCheckerCheckJumpError>;

  bool isLoop(const Stmt *stmt) const;
  llvm::StringRef jumpLabel(const Stmt *stmt) const;
  ResultTargetFound checkTargetFound(const JumpTarget *target, llvm::StringRef label,
                                     bool isBreak) const;
  ResultTargetFound checkContinueTarget(const JumpTarget &target, llvm::StringRef label,
                                        bool isBreak) const;
  ResultLabelIsFresh checkLabelFresh(llvm::ArrayRef<llvm::StringRef> labels,
                                     const LabeledStmt *labeled) const;
  ResultBodyCanLabel checkLabelBody(const Stmt *body) const;
  void walkLabeledBlock(Stmt *body, llvm::ArrayRef<llvm::StringRef> labels);

  SemanticState &state;
  StmtChecker *stmtChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
};

}
