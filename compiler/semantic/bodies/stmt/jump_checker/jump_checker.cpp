#include "compiler/semantic/bodies/stmt/jump_checker/jump_checker.h"
#include "compiler/semantic/bodies/stmt/stmt_checker/stmt_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

JumpChecker::JumpChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->stmtChecker = &all.stmtChecker;
}

bool JumpChecker::isLoop(const Stmt *stmt) {
  return isa<WhileStmt>(stmt) || isa<DoWhileStmt>(stmt) || isa<ForStmt>(stmt) ||
         isa<ForOfStmt>(stmt);
}

void JumpChecker::pushTarget(JumpTarget::Kind kind) {
  JumpTarget target{kind, this->state.getBlockPosition().takePendingLabels(), Flow()};
  target.breaks.unreachable = true;
  this->state.getBlockPosition().enterJumpTarget(std::move(target));
}

JumpChecker::ResultJumpTargeted JumpChecker::checkJump(Stmt *stmt) {
  bool isBreak = isa<BreakStmt>(stmt);
  llvm::StringRef label = isBreak ? cast<BreakStmt>(stmt)->label : cast<ContinueStmt>(stmt)->label;
  const char *keyword = isBreak ? "break" : "continue";
  JumpTarget *target = this->state.getBlockPosition().findJumpTarget(label, isBreak);
  if (!target && !label.empty()) {
    std::string message = ErrorTextJump::noLabel(label);

    return err(JumpCheckerCheckJumpError{
        .code = JumpCheckerCheckJumpErrorCode::LABEL_NOT_FOUND,
        .message = message,
    });
  }
  if (!target && isBreak) {
    std::string message = ErrorTextJump::breakOut();

    return err(JumpCheckerCheckJumpError{
        .code = JumpCheckerCheckJumpErrorCode::BREAK_OUTSIDE,
        .message = message,
    });
  }
  if (!target) {
    std::string message = ErrorTextJump::continueOut();

    return err(JumpCheckerCheckJumpError{
        .code = JumpCheckerCheckJumpErrorCode::CONTINUE_OUTSIDE,
        .message = message,
    });
  }
  if (!isBreak && target->kind != JumpTarget::Kind::Loop) {
    std::string message = ErrorTextJump::notLoop(keyword, label);

    return err(JumpCheckerCheckJumpError{
        .code = JumpCheckerCheckJumpErrorCode::CONTINUE_NOT_LOOP,
        .message = message,
    });
  }

  if (isBreak && target->kind != JumpTarget::Kind::Loop) {
    target->breaks = FlowChecker::join(target->breaks, this->state.getFlowState().getFlow());
  }

  return ok(stmt);
}

JumpChecker::ResultLabelIsFresh JumpChecker::labelFresh(llvm::ArrayRef<llvm::StringRef> labels,
                                                        const LabeledStmt *labeled) const {
  bool isDuplicate = llvm::is_contained(labels, labeled->label) ||
                     this->state.getBlockPosition().hasLabel(labeled->label);
  if (isDuplicate) {
    std::string message = ErrorTextLabel::duplicate(labeled->label);

    return err(JumpCheckerLabelFreshError{
        .code = JumpCheckerLabelFreshErrorCode::DUPLICATE_LABEL,
        .message = message,
    });
  }

  return ok(labeled);
}

JumpChecker::ResultBodyCanLabel JumpChecker::labelBody(const Stmt *body) {
  if (isa<VarDeclStmt>(body) || isa<FunctionDecl>(body) || isa<TypeAliasDecl>(body) ||
      isa<EnumDecl>(body)) {
    std::string message = ErrorTextLabelBody::declared();

    return err(JumpCheckerLabelBodyError{
        .code = JumpCheckerLabelBodyErrorCode::DECLARATION_LABELED,
        .message = message,
    });
  }

  return ok(body);
}

void JumpChecker::walkLabeled(LabeledStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels;
  Stmt *body = stmt;
  while (auto *labeled = dyn_cast<LabeledStmt>(body)) {
    ResultLabelIsFresh fresh = this->labelFresh(labels, labeled);
    if (fresh.isErr()) {
      this->state.getDiagnostics().error(labeled->loc(), fresh.error().message);
    }
    labels.push_back(labeled->label);
    body = labeled->body;
  }
  if (JumpChecker::isLoop(body) || isa<SwitchStmt>(body)) {
    this->state.getBlockPosition().rememberPendingLabels(labels);
    this->stmtChecker->walkStmt(body);
    return;
  }
  ResultBodyCanLabel labelable = JumpChecker::labelBody(body);
  if (labelable.isErr()) {
    this->state.getDiagnostics().error(body->loc(), labelable.error().message);
    return;
  }
  this->state.getBlockPosition().rememberPendingLabels(labels);
  this->pushTarget(JumpTarget::Kind::Block);
  this->stmtChecker->walkStmt(body);
  Flow breaks = this->state.getBlockPosition().leaveJumpTarget().breaks;
  this->state.getFlowState().replaceFlow(
      FlowChecker::join(this->state.getFlowState().getFlow(), breaks));
}

}
