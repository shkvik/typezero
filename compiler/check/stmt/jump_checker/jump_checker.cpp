#include "compiler/check/stmt/jump_checker/jump_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

static bool isLoop(const Stmt *stmt) {
  return isa<WhileStmt>(stmt) || isa<DoWhileStmt>(stmt) || isa<ForStmt>(stmt) ||
         isa<ForOfStmt>(stmt);
}

JumpChecker::JumpChecker(Checker &checker) : checker_(checker) {}

void JumpChecker::pushTarget(Checker::JumpTarget::Kind kind) {
  Checker::JumpTarget target{kind, std::move(checker_.pendingLabels_), Checker::Flow()};
  checker_.pendingLabels_.clear();
  target.breaks.unreachable = true;
  checker_.targets_.push_back(std::move(target));
}

void JumpChecker::checkJump(Stmt *stmt) {
  bool isBreak = isa<BreakStmt>(stmt);
  llvm::StringRef label = isBreak ? cast<BreakStmt>(stmt)->label : cast<ContinueStmt>(stmt)->label;
  const char *keyword = isBreak ? "break" : "continue";
  Checker::JumpTarget *target = nullptr;
  for (Checker::JumpTarget &t : llvm::reverse(checker_.targets_)) {
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
      checker_.diags_.error(stmt->loc(), "cannot find label '" + label + "'");
    } else if (isBreak) {
      checker_.diags_.error(stmt->loc(), "'break' can only be used inside a loop or a 'switch'");
    } else {
      checker_.diags_.error(stmt->loc(), "'continue' can only be used inside a loop");
    }
    return;
  }
  if (!isBreak && target->kind != Checker::JumpTarget::Kind::Loop) {
    checker_.diags_.error(stmt->loc(), llvm::Twine("'") + keyword + " " + label +
                                           "' needs a loop, and '" + label +
                                           "' does not label one");
    return;
  }
  if (isBreak && target->kind != Checker::JumpTarget::Kind::Loop) {
    target->breaks = FlowChecker::join(target->breaks, checker_.flow_);
  }
}

void JumpChecker::checkLabeled(LabeledStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels;
  Stmt *body = stmt;
  while (auto *labeled = dyn_cast<LabeledStmt>(body)) {
    bool duplicate = llvm::is_contained(labels, labeled->label) ||
                     llvm::any_of(checker_.targets_, [&](const Checker::JumpTarget &t) {
                       return llvm::is_contained(t.labels, labeled->label);
                     });
    if (duplicate) {
      checker_.diags_.error(labeled->loc(), "duplicate label '" + labeled->label + "'");
    }
    labels.push_back(labeled->label);
    body = labeled->body;
  }
  if (isLoop(body) || isa<SwitchStmt>(body)) {
    checker_.pendingLabels_ = labels; // for the loop or switch to take
    checker_.stmtChecker().checkStmt(body);
    return;
  }
  if (isa<VarDeclStmt>(body) || isa<FunctionDecl>(body) || isa<TypeAliasDecl>(body) ||
      isa<EnumDecl>(body)) {
    checker_.diags_.error(body->loc(), "a declaration cannot have a label");
    return;
  }
  // `label: { ... break label; ... }`
  checker_.pendingLabels_ = labels;
  pushTarget(Checker::JumpTarget::Kind::Block);
  checker_.stmtChecker().checkStmt(body);
  Checker::Flow breaks = checker_.targets_.back().breaks;
  checker_.targets_.pop_back();
  checker_.flow_ = FlowChecker::join(checker_.flow_, breaks);
}

} // namespace tz
