#include "compiler/check/errors/throw_checker/throw_checker.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/loop_bounds/loop_bounds.h"
#include "compiler/check/flow/loop_scanner/loop_scanner.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/whole_program/flows/flows.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

ThrowChecker::ThrowChecker(Checker &checker) : checker_(checker) {}

void ThrowChecker::checkThrow(ThrowStmt *stmt) {
  checker_.diags_.error(stmt->loc(), "there is no 'throw' in tz: an error is a value; return it, "
                                     "'return err(...)', and give the function a 'Result'");
  stmt->value = checker_.exprChecker().checkExpr(stmt->value);
}

Checker::Flow ThrowChecker::withoutChanges(const Checker::Flow &flow,
                                           const Checker::LoopScan &scan) {
  Checker::Flow saved = checker_.flow_;
  checker_.flow_ = flow;
  Checker::Flow result = checker_.loopBounds().loopHead(scan, nullptr, false);
  checker_.flow_ = std::move(saved);
  result.unreachable = flow.unreachable;

  return result;
}

void ThrowChecker::checkTry(TryStmt *stmt) {
  if (stmt->handler || !checker_.libraryCalls().inLibrary(stmt->loc())) {
    checker_.diags_.error(stmt->loc(), "there is no 'try' in tz: an error is a value; a function "
                                       "that can fail gives a 'Result', checked with 'if'");
  }
  Checker::LoopScan blockScan = checker_.loopScanner().scanLoop({stmt->block}, {});
  Checker::Flow inBlock = withoutChanges(checker_.flow_, blockScan);
  std::vector<const Stmt *> guarded{stmt->block};
  if (stmt->handler) {
    guarded.push_back(stmt->handler);
  }
  Checker::LoopScan guardedScan = checker_.loopScanner().scanLoop(guarded, {});
  Checker::Flow inGuarded = withoutChanges(checker_.flow_, guardedScan);

  ++checker_.tryDepth_;
  checker_.stmtChecker().checkStmt(stmt->block);
  Checker::Flow normal = checker_.flow_;
  if (stmt->handler) {
    checker_.flow_ = inBlock;
    checker_.pushScope();
    VarDeclarator &param = stmt->param;
    if (!param.name.empty()) {
      param.sym = checker_.scopes().declareVar(param.name, param.loc, Type::error(), false);
    }
    checker_.stmtChecker().checkStmt(stmt->handler);
    checker_.popScope();
    normal = FlowChecker::join(normal, checker_.flow_);
  }
  --checker_.tryDepth_;
  if (!stmt->finalizer) {
    checker_.flow_ = normal;
    return;
  }
  checker_.flow_ = FlowChecker::join(normal, inGuarded);
  checker_.stmtChecker().checkStmt(stmt->finalizer);
  checker_.flow_.unreachable |= normal.unreachable;
  Checker::LoopScan finallyScan = checker_.loopScanner().scanLoop({stmt->finalizer}, {});
  for (Checker::JumpTarget &target : checker_.targets_) {
    target.breaks = withoutChanges(target.breaks, finallyScan);
  }
}

}
