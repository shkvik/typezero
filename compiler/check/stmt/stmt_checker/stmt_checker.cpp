#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/truth_checker/truth_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/predicate_checker/predicate_checker.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/stmt/jump_checker/jump_checker.h"
#include "compiler/check/stmt/loop_checker/loop_checker.h"
#include "compiler/check/stmt/return_checker/return_checker.h"
#include "compiler/check/stmt/switch_checker/switch_checker.h"
#include "compiler/check/stmt/var_checker/var_checker.h"
#include "compiler/library/result/result_checker/result_checker.h"
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

StmtChecker::StmtChecker(Checker &checker) : checker_(checker) {}

void StmtChecker::checkStmt(Stmt *stmt) {
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    checker_.pushScope();
    checker_.closureChecker().checkStatements(cast<BlockStmt>(stmt));
    checker_.popScope();
    break;
  case Stmt::Kind::VarDecl:
    checker_.varChecker().checkVarDecl(cast<VarDeclStmt>(stmt));
    break;
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
    break;
  case Stmt::Kind::Class:
    checker_.classChecker().checkClassStmt(cast<ClassDecl>(stmt));
    break;
  case Stmt::Kind::Function:
    if (!cast<FunctionDecl>(stmt)->group) {
      checker_.diags_.error(stmt->loc(),
                            "a function can be declared only directly in a block; in a "
                            "'switch', declare it before the 'switch'");
    }
    break;
  case Stmt::Kind::Expr: {
    auto *s = cast<ExprStmt>(stmt);
    if (auto *yield = dyn_cast<YieldExpr>(s->expr)) {
      yield->discarded = true;
    }
    s->expr = checker_.exprChecker().checkExpr(s->expr);
    checker_.resultChecker().checkDiscarded(s->expr, false, s->loc());
    if (auto *call = dyn_cast<CallExpr>(s->expr)) {
      const FuncSymbol *callee = call->func ? call->func : call->method;
      if (callee && callee->asserts) {
        Checker::Flow ignored;
        checker_.predicateChecker().applyPredicate(call, checker_.flow_, ignored);
      }
    }
    if (ReturnChecker::isExit(s)) {
      checker_.flow_.unreachable = true;
    }
    break;
  }
  case Stmt::Kind::If:
    ++checker_.branchDepth_;
    checkIf(cast<IfStmt>(stmt));
    --checker_.branchDepth_;
    break;
  case Stmt::Kind::While:
    ++checker_.branchDepth_;
    checker_.loopChecker().checkWhile(cast<WhileStmt>(stmt));
    --checker_.branchDepth_;
    break;
  case Stmt::Kind::DoWhile:
    ++checker_.branchDepth_;
    checker_.loopChecker().checkDoWhile(cast<DoWhileStmt>(stmt));
    --checker_.branchDepth_;
    break;
  case Stmt::Kind::For:
    ++checker_.branchDepth_;
    checker_.loopChecker().checkFor(cast<ForStmt>(stmt));
    --checker_.branchDepth_;
    break;
  case Stmt::Kind::ForOf:
    ++checker_.branchDepth_;
    checker_.loopChecker().checkForOf(cast<ForOfStmt>(stmt));
    --checker_.branchDepth_;
    break;
  case Stmt::Kind::Switch:
    ++checker_.branchDepth_;
    checker_.switchChecker().checkSwitch(cast<SwitchStmt>(stmt));
    --checker_.branchDepth_;
    break;
  case Stmt::Kind::Labeled:
    checker_.jumpChecker().checkLabeled(cast<LabeledStmt>(stmt));
    break;
  case Stmt::Kind::Return:
    checker_.returnChecker().checkReturn(cast<ReturnStmt>(stmt));
    checker_.flow_.unreachable = true;
    break;
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue:
    checker_.jumpChecker().checkJump(stmt);
    checker_.flow_.unreachable = true;
    break;
  case Stmt::Kind::Empty:
  case Stmt::Kind::Debugger:
    break;
  }
}

void StmtChecker::checkIf(IfStmt *stmt) {
  Checker::Flow whenTrue, whenFalse;
  stmt->cond = checker_.truthChecker().checkCondition(stmt->cond, whenTrue, whenFalse);
  if (std::optional<bool> constant = checker_.checkingInstance_
                                         ? checker_.genericChecker().constantCondition(stmt->cond)
                                         : std::nullopt) {
    auto *lit = checker_.ctx_.make<BoolLitExpr>(stmt->cond->loc(), *constant);
    lit->type = Type::boolTy();
    stmt->cond = lit;
    if (*constant) {
      checker_.flow_ = whenTrue;
      checkBody(stmt->thenStmt);
      stmt->elseStmt = nullptr;
    } else {
      stmt->thenStmt = checker_.ctx_.make<EmptyStmt>(stmt->thenStmt->loc());
      checker_.flow_ = whenFalse;
      if (stmt->elseStmt) {
        checkBody(stmt->elseStmt);
      }
    }
    return;
  }
  checker_.flow_ = whenTrue;
  checkBody(stmt->thenStmt);
  Checker::Flow afterThen = checker_.flow_;
  checker_.flow_ = whenFalse;
  if (stmt->elseStmt) {
    checkBody(stmt->elseStmt);
  }
  checker_.flow_ = FlowChecker::join(afterThen, checker_.flow_);
}

void StmtChecker::checkBody(Stmt *body) {
  if (isa<VarDeclStmt>(body) || isa<TypeAliasDecl>(body) || isa<EnumDecl>(body) ||
      isa<FunctionDecl>(body)) {
    checker_.diags_.error(body->loc(),
                          "a declaration cannot be the whole body here; wrap it in '{ }'");
    return;
  }
  checkStmt(body);
}

}
