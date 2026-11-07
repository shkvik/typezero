#include "compiler/check/stmt/jump_checker/jump_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

static bool isLoop(const Stmt *stmt) {
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

void JumpChecker::checkJump(Stmt *stmt) {
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
  if (!target) {
    if (!label.empty()) {
      this->checker.diags_.error(stmt->loc(), "cannot find label '" + label + "'");
    } else if (isBreak) {
      this->checker.diags_.error(stmt->loc(),
                                 "'break' can only be used inside a loop or a 'switch'");
    } else {
      this->checker.diags_.error(stmt->loc(), "'continue' can only be used inside a loop");
    }
    return;
  }
  if (!isBreak && target->kind != Checker::JumpTarget::Kind::Loop) {
    this->checker.diags_.error(stmt->loc(), llvm::Twine("'") + keyword + " " + label +
                                                "' needs a loop, and '" + label +
                                                "' does not label one");
    return;
  }
  if (isBreak && target->kind != Checker::JumpTarget::Kind::Loop) {
    target->breaks = FlowChecker::join(target->breaks, this->checker.flow_);
  }
}

void JumpChecker::checkLabeled(LabeledStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels;
  Stmt *body = stmt;
  while (auto *labeled = dyn_cast<LabeledStmt>(body)) {
    bool duplicate = llvm::is_contained(labels, labeled->label) ||
                     llvm::any_of(this->checker.targets_, [&](const Checker::JumpTarget &t) {
                       return llvm::is_contained(t.labels, labeled->label);
                     });
    if (duplicate) {
      this->checker.diags_.error(labeled->loc(), "duplicate label '" + labeled->label + "'");
    }
    labels.push_back(labeled->label);
    body = labeled->body;
  }
  if (isLoop(body) || isa<SwitchStmt>(body)) {
    this->checker.pendingLabels_ = labels;
    this->checker.stmtChecker().checkStmt(body);
    return;
  }
  if (isa<VarDeclStmt>(body) || isa<FunctionDecl>(body) || isa<TypeAliasDecl>(body) ||
      isa<EnumDecl>(body)) {
    this->checker.diags_.error(body->loc(), "a declaration cannot have a label");
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
