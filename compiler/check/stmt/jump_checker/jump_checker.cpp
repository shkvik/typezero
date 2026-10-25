#include "compiler/check/stmt/jump_checker/jump_checker.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

static bool isLoop(const Stmt *stmt) {
  return isa<WhileStmt>(stmt) || isa<DoWhileStmt>(stmt) || isa<ForStmt>(stmt) ||
         isa<ForOfStmt>(stmt);
}

JumpChecker::JumpChecker(Sema &sema) : sema_(sema) {}

void JumpChecker::pushTarget(Sema::JumpTarget::Kind kind) {
  Sema::JumpTarget target{kind, std::move(sema_.pendingLabels_), Sema::Flow()};
  sema_.pendingLabels_.clear();
  target.breaks.unreachable = true;
  sema_.targets_.push_back(std::move(target));
}

void JumpChecker::checkJump(Stmt *stmt) {
  bool isBreak = isa<BreakStmt>(stmt);
  llvm::StringRef label = isBreak ? cast<BreakStmt>(stmt)->label : cast<ContinueStmt>(stmt)->label;
  const char *keyword = isBreak ? "break" : "continue";
  Sema::JumpTarget *target = nullptr;
  for (Sema::JumpTarget &t : llvm::reverse(sema_.targets_)) {
    bool matches = label.empty() ? (isBreak ? t.kind != Sema::JumpTarget::Kind::Block
                                            : t.kind == Sema::JumpTarget::Kind::Loop)
                                 : llvm::is_contained(t.labels, label);
    if (matches) {
      target = &t;
      break;
    }
  }
  if (!target) {
    if (!label.empty()) {
      sema_.diags_.error(stmt->loc(), "cannot find label '" + label + "'");
    } else if (isBreak) {
      sema_.diags_.error(stmt->loc(), "'break' can only be used inside a loop or a 'switch'");
    } else {
      sema_.diags_.error(stmt->loc(), "'continue' can only be used inside a loop");
    }
    return;
  }
  if (!isBreak && target->kind != Sema::JumpTarget::Kind::Loop) {
    sema_.diags_.error(stmt->loc(), llvm::Twine("'") + keyword + " " + label + "' needs a loop, and '" +
                                  label + "' does not label one");
    return;
  }
  if (isBreak && target->kind != Sema::JumpTarget::Kind::Loop) {
    target->breaks = FlowChecker::join(target->breaks, sema_.flow_);
  }
}

void JumpChecker::checkLabeled(LabeledStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels;
  Stmt *body = stmt;
  while (auto *labeled = dyn_cast<LabeledStmt>(body)) {
    bool duplicate = llvm::is_contained(labels, labeled->label) ||
                     llvm::any_of(sema_.targets_, [&](const Sema::JumpTarget &t) {
                       return llvm::is_contained(t.labels, labeled->label);
                     });
    if (duplicate) {
      sema_.diags_.error(labeled->loc(), "duplicate label '" + labeled->label + "'");
    }
    labels.push_back(labeled->label);
    body = labeled->body;
  }
  if (isLoop(body) || isa<SwitchStmt>(body)) {
    sema_.pendingLabels_ = labels; // for the loop or switch to take
    sema_.stmtChecker().checkStmt(body);
    return;
  }
  if (isa<VarDeclStmt>(body) || isa<FunctionDecl>(body) || isa<TypeAliasDecl>(body) ||
      isa<EnumDecl>(body)) {
    sema_.diags_.error(body->loc(), "a declaration cannot have a label");
    return;
  }
  // `label: { ... break label; ... }`
  sema_.pendingLabels_ = labels;
  pushTarget(Sema::JumpTarget::Kind::Block);
  sema_.stmtChecker().checkStmt(body);
  Sema::Flow breaks = sema_.targets_.back().breaks;
  sema_.targets_.pop_back();
  sema_.flow_ = FlowChecker::join(sema_.flow_, breaks);
}

} // namespace tz
