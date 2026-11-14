#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/loops/code_loops_registry_errors.h"
#include "compiler/semantic/flow/tree_place/tree_place.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class PlainLoops {
public:
  explicit PlainLoops(SemanticState &owner);

  void finishLabeledBlock();
  void walkFor(const WalkFor &task);
  void checkJump(const CheckJump &task);
  void walkWhile(const WalkWhile &task);
  void finishFor(const FinishFor &task);
  void walkLabeled(const WalkLabeled &task);
  void walkDoWhile(const WalkDoWhile &task);
  void afterForInit(const AfterForInit &task);
  void afterForBody(const AfterForBody &task);
  void afterForStep(const AfterForStep &task);
  void leaveLoopBody(const LeaveLoopBody &task);
  void restoreLoopFlow(const RestoreLoopFlow &task);
  void afterDoWhileBody(const AfterDoWhileBody &task);
  void afterForCondition(const AfterForCondition &task);
  void afterWhileCondition(const AfterWhileCondition &task);
  void afterDoWhileCondition(const AfterDoWhileCondition &task);

private:
  using ResultJumpTargeted = Result<Stmt *, CodeLoopsRegistryCheckJumpError>;
  using ResultBodyCanLabel = Result<const Stmt *, CodeLoopsRegistryLabelBodyError>;
  using ResultTargetFound = Result<const JumpTarget *, CodeLoopsRegistryCheckJumpError>;
  using ResultLabelIsFresh = Result<const LabeledStmt *, CodeLoopsRegistryLabelFreshError>;

  void startForBody(ForDraft draft);
  void pushTarget(JumpTarget::Kind kind);
  void startScannedBody(Stmt *body, const LoopScan &scan);
  void enterStepFlow(const Stmt *body, llvm::ArrayRef<llvm::StringRef> labels, const Flow &head,
                     const Flow &whenTrue);


  bool isLoop(const Stmt *stmt) const;
  bool isContinuingTo(const Stmt *body, llvm::ArrayRef<llvm::StringRef> labels) const;
  bool isContinuingLoop(const ContinueStmt *next, const TreePlace &place,
                        llvm::ArrayRef<llvm::StringRef> labels) const;


  ResultTargetFound checkTargetFound(const JumpTarget *target, llvm::StringRef label,
                                     bool isBreak) const;
  ResultTargetFound checkContinueTarget(const JumpTarget &target, llvm::StringRef label,
                                        bool isBreak) const;


  ResultJumpTargeted checkJumpTarget(Stmt *stmt);
  ResultBodyCanLabel checkLabelBody(const Stmt *body) const;
  ResultLabelIsFresh checkLabelFresh(llvm::ArrayRef<llvm::StringRef> labels,
                                     const LabeledStmt *labeled) const;


  llvm::StringRef jumpLabel(const Stmt *stmt) const;
  Flow flowAfter(const LoopScan &scan, const Flow &whenFalse, const Flow &head) const;

  SemanticState &state;
};

}
