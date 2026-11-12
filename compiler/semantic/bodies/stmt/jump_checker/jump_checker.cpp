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

namespace {

constexpr const char *kContinueKeyword = "continue";

}

JumpChecker::JumpChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->stmtChecker = &all.stmtChecker;
  this->flowChecker = &all.flowChecker;
}

bool JumpChecker::isLoop(const Stmt *stmt) const {
  return isa<WhileStmt>(stmt) || isa<DoWhileStmt>(stmt) || isa<ForStmt>(stmt) ||
         isa<ForOfStmt>(stmt);
}

void JumpChecker::pushTarget(JumpTarget::Kind kind) {
  auto labels = this->state.getBlockPosition().takePendingLabels();
  auto breaks = Flow();
  breaks.unreachable = true;
  auto target = JumpTarget{.kind = kind, .labels = std::move(labels), .breaks = std::move(breaks)};
  this->state.getBlockPosition().enterJumpTarget(std::move(target));
}

llvm::StringRef JumpChecker::jumpLabel(const Stmt *stmt) const {
  auto *exit = dyn_cast<BreakStmt>(stmt);
  if (exit != nullptr) {
    return exit->label;
  }

  return cast<ContinueStmt>(stmt)->label;
}

JumpChecker::ResultJumpTargeted JumpChecker::checkJump(Stmt *stmt) {
  bool isBreak = isa<BreakStmt>(stmt);
  auto label = this->jumpLabel(stmt);
  auto *target = this->state.getBlockPosition().findJumpTarget(label, isBreak);
  auto found = this->checkTargetFound(target, label, isBreak);
  if (found.isErr()) {
    return err(found.error());
  }
  auto looped = this->checkContinueTarget(*target, label, isBreak);
  if (looped.isErr()) {
    return err(looped.error());
  }

  if (isBreak && target->kind != JumpTarget::Kind::Loop) {
    const auto &here = this->state.getFlowState().getFlow();
    target->breaks = this->flowChecker->join(target->breaks, here);
  }

  return ok(stmt);
}

JumpChecker::ResultTargetFound
JumpChecker::checkTargetFound(const JumpTarget *target, llvm::StringRef label, bool isBreak) const {
  if (target == nullptr && !label.empty()) {
    auto message = ErrorTextJump::noLabel(label);

    return err(JumpCheckerCheckJumpError{
        .code = JumpCheckerCheckJumpErrorCode::LABEL_NOT_FOUND,
        .message = message,
    });
  }
  if (target == nullptr && isBreak) {
    auto message = ErrorTextJump::breakOut();

    return err(JumpCheckerCheckJumpError{
        .code = JumpCheckerCheckJumpErrorCode::BREAK_OUTSIDE,
        .message = message,
    });
  }
  if (target == nullptr) {
    auto message = ErrorTextJump::continueOut();

    return err(JumpCheckerCheckJumpError{
        .code = JumpCheckerCheckJumpErrorCode::CONTINUE_OUTSIDE,
        .message = message,
    });
  }

  return ok(target);
}

JumpChecker::ResultTargetFound JumpChecker::checkContinueTarget(const JumpTarget &target,
                                                                llvm::StringRef label,
                                                                bool isBreak) const {
  if (!isBreak && target.kind != JumpTarget::Kind::Loop) {
    auto message = ErrorTextJump::notLoop(kContinueKeyword, label);

    return err(JumpCheckerCheckJumpError{
        .code = JumpCheckerCheckJumpErrorCode::CONTINUE_NOT_LOOP,
        .message = message,
    });
  }

  return ok(&target);
}

JumpChecker::ResultLabelIsFresh JumpChecker::checkLabelFresh(llvm::ArrayRef<llvm::StringRef> labels,
                                                             const LabeledStmt *labeled) const {
  bool isDuplicate = llvm::is_contained(labels, labeled->label) ||
                     this->state.getBlockPosition().hasLabel(labeled->label);
  if (isDuplicate) {
    auto message = ErrorTextLabel::duplicate(labeled->label);

    return err(JumpCheckerLabelFreshError{
        .code = JumpCheckerLabelFreshErrorCode::DUPLICATE_LABEL,
        .message = message,
    });
  }

  return ok(labeled);
}

JumpChecker::ResultBodyCanLabel JumpChecker::checkLabelBody(const Stmt *body) const {
  bool isDeclaration = isa<VarDeclStmt>(body) || isa<FunctionDecl>(body) ||
                       isa<TypeAliasDecl>(body) || isa<EnumDecl>(body);
  if (isDeclaration) {
    auto message = ErrorTextLabelBody::declared();

    return err(JumpCheckerLabelBodyError{
        .code = JumpCheckerLabelBodyErrorCode::DECLARATION_LABELED,
        .message = message,
    });
  }

  return ok(body);
}

void JumpChecker::walkLabeled(LabeledStmt *stmt) {
  auto labels = llvm::SmallVector<llvm::StringRef, 1>();
  Stmt *body = stmt;
  auto *labeled = dyn_cast<LabeledStmt>(body);
  while (labeled != nullptr) {
    auto fresh = this->checkLabelFresh(labels, labeled);
    if (fresh.isErr()) {
      this->state.getDiagnostics().error(labeled->loc(), fresh.error().message);
    }
    labels.push_back(labeled->label);
    body = labeled->body;
    labeled = dyn_cast<LabeledStmt>(body);
  }
  if (this->isLoop(body) || isa<SwitchStmt>(body)) {
    this->state.getBlockPosition().rememberPendingLabels(labels);
    this->stmtChecker->walkStmt(body);
    return;
  }
  auto labelable = this->checkLabelBody(body);
  if (labelable.isErr()) {
    this->state.getDiagnostics().error(body->loc(), labelable.error().message);
    return;
  }
  this->walkLabeledBlock(body, labels);
}

void JumpChecker::walkLabeledBlock(Stmt *body, llvm::ArrayRef<llvm::StringRef> labels) {
  this->state.getBlockPosition().rememberPendingLabels(labels);
  this->pushTarget(JumpTarget::Kind::Block);
  this->stmtChecker->walkStmt(body);
  auto breaks = this->state.getBlockPosition().leaveJumpTarget().breaks;
  const auto &after = this->state.getFlowState().getFlow();
  auto joined = this->flowChecker->join(after, breaks);
  this->state.getFlowState().replaceFlow(joined);
}

}
