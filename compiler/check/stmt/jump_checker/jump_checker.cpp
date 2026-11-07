#include "compiler/check/stmt/jump_checker/jump_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

bool JumpChecker::isLoop(const Stmt *stmt) {
  return isa<WhileStmt>(stmt) || isa<DoWhileStmt>(stmt) || isa<ForStmt>(stmt) ||
         isa<ForOfStmt>(stmt);
}

JumpChecker::JumpChecker(Checker &owner)
    : checker(owner) {}

void JumpChecker::pushTarget(Checker::JumpTarget::Kind kind) {
  Checker::JumpTarget target{kind, std::move(this->checker.pendingLabels_), Checker::Flow()};
  this->checker.pendingLabels_.clear();
  target.breaks.unreachable = true;
  this->checker.targets_.push_back(std::move(target));
}

JumpChecker::ResultJumpTargeted JumpChecker::checkJump(Stmt *stmt) {
  bool isBreak = isa<BreakStmt>(stmt);
  llvm::StringRef label = isBreak ? cast<BreakStmt>(stmt)->label : cast<ContinueStmt>(stmt)->label;
  const char *keyword = isBreak ? "break" : "continue";
  Checker::JumpTarget *target = nullptr;
  for (Checker::JumpTarget &t : llvm::reverse(this->checker.targets_)) {
    bool matches = label.empty() ? (isBreak ? t.kind != Checker::JumpTarget::Kind::Block
                                            : t.kind == Checker::JumpTarget::Kind::Loop)
                                 : llvm::is_contained(t.labels, label);
    if (matches) {
      target = &t;
      break;
    }
  }
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
  if (!isBreak && target->kind != Checker::JumpTarget::Kind::Loop) {
    std::string message = ErrorTextJump::notLoop(keyword, label);

    return err(JumpCheckerCheckJumpError{
        .code = JumpCheckerCheckJumpErrorCode::CONTINUE_NOT_LOOP,
        .message = message,
    });
  }

  if (isBreak && target->kind != Checker::JumpTarget::Kind::Loop) {
    target->breaks = FlowChecker::join(target->breaks, this->checker.flow_);
  }

  return ok(stmt);
}

JumpChecker::ResultLabelIsFresh JumpChecker::labelFresh(llvm::ArrayRef<llvm::StringRef> labels,
                                                        const LabeledStmt *labeled) const {
  auto hasLabel = [&](const Checker::JumpTarget &t) {
    return llvm::is_contained(t.labels, labeled->label);
  };
  bool duplicate =
      llvm::is_contained(labels, labeled->label) || llvm::any_of(this->checker.targets_, hasLabel);
  if (duplicate) {
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

void JumpChecker::checkLabeled(LabeledStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels;
  Stmt *body = stmt;
  while (auto *labeled = dyn_cast<LabeledStmt>(body)) {
    ResultLabelIsFresh fresh = this->labelFresh(labels, labeled);
    if (fresh.isErr()) {
      this->checker.diags_.error(labeled->loc(), fresh.error().message);
    }
    labels.push_back(labeled->label);
    body = labeled->body;
  }
  if (JumpChecker::isLoop(body) || isa<SwitchStmt>(body)) {
    this->checker.pendingLabels_ = labels;
    this->checker.stmtChecker().checkStmt(body);
    return;
  }
  ResultBodyCanLabel labelable = JumpChecker::labelBody(body);
  if (labelable.isErr()) {
    this->checker.diags_.error(body->loc(), labelable.error().message);
    return;
  }
  this->checker.pendingLabels_ = labels;
  this->pushTarget(Checker::JumpTarget::Kind::Block);
  this->checker.stmtChecker().checkStmt(body);
  Checker::Flow breaks = this->checker.targets_.back().breaks;
  this->checker.targets_.pop_back();
  this->checker.flow_ = FlowChecker::join(this->checker.flow_, breaks);
}

}
