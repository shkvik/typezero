#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/check/stmt/return_checker/return_checker.h"
#include "compiler/check/stmt/jump_checker/jump_checker.h"
#include "compiler/check/stmt/loop_checker/loop_checker.h"
#include "compiler/check/stmt/switch_checker/switch_checker.h"
#include "compiler/check/stmt/var_checker/var_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/errors/throw_checker/throw_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/flow/predicate_checker/predicate_checker.h"
#include "compiler/check/expr/truth_checker/truth_checker.h"
#include "compiler/library/result/result_checker/result_checker.h"
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

StmtChecker::StmtChecker(Sema &sema) : sema_(sema) {}

void StmtChecker::checkStmt(Stmt *stmt) {
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    sema_.pushScope();
    sema_.closureChecker().checkStatements(cast<BlockStmt>(stmt));
    sema_.popScope();
    break;
  case Stmt::Kind::VarDecl:
    sema_.varChecker().checkVarDecl(cast<VarDeclStmt>(stmt));
    break;
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
    break; // declared with the block (declareTypes)
  case Stmt::Kind::Class:
    sema_.classChecker().checkClassStmt(cast<ClassDecl>(stmt));
    break;
  case Stmt::Kind::Function:
    // check() handles top-level functions, and checkStatements those of a
    // block.
    if (!cast<FunctionDecl>(stmt)->group) {
      sema_.diags_.error(stmt->loc(), "a function can be declared only directly in a block; in a "
                                "'switch', declare it before the 'switch'");
    }
    break;
  case Stmt::Kind::Expr: {
    auto *s = cast<ExprStmt>(stmt);
    // `yield x;` drops what next() sends.
    if (auto *yield = dyn_cast<YieldExpr>(s->expr)) {
      yield->discarded = true;
    }
    s->expr = sema_.exprChecker().checkExpr(s->expr);
    sema_.resultChecker().checkDiscarded(s->expr, /*callback=*/false, s->loc());
    // `assertIsCircle(s);` narrows from here on, as `asserts s is Circle`
    // says, and as its body shows.
    if (auto *call = dyn_cast<CallExpr>(s->expr)) {
      const FuncSymbol *callee = call->func ? call->func : call->method;
      if (callee && callee->asserts) {
        Sema::Flow ignored;
        sema_.predicateChecker().applyPredicate(call, sema_.flow_, ignored);
      }
    }
    // Code after `process.exit()` never runs, as after `return`: in
    // `if (!file) process.exit(1)` the rest knows that file is there.
    if (ReturnChecker::isExit(s)) {
      sema_.flow_.unreachable = true;
    }
    break;
  }
  case Stmt::Kind::If:
    ++sema_.branchDepth_;
    checkIf(cast<IfStmt>(stmt));
    --sema_.branchDepth_;
    break;
  case Stmt::Kind::While:
    ++sema_.branchDepth_;
    sema_.loopChecker().checkWhile(cast<WhileStmt>(stmt));
    --sema_.branchDepth_;
    break;
  case Stmt::Kind::DoWhile:
    ++sema_.branchDepth_;
    sema_.loopChecker().checkDoWhile(cast<DoWhileStmt>(stmt));
    --sema_.branchDepth_;
    break;
  case Stmt::Kind::For:
    ++sema_.branchDepth_;
    sema_.loopChecker().checkFor(cast<ForStmt>(stmt));
    --sema_.branchDepth_;
    break;
  case Stmt::Kind::ForOf:
    ++sema_.branchDepth_;
    sema_.loopChecker().checkForOf(cast<ForOfStmt>(stmt));
    --sema_.branchDepth_;
    break;
  case Stmt::Kind::Switch:
    ++sema_.branchDepth_;
    sema_.switchChecker().checkSwitch(cast<SwitchStmt>(stmt));
    --sema_.branchDepth_;
    break;
  case Stmt::Kind::Labeled:
    sema_.jumpChecker().checkLabeled(cast<LabeledStmt>(stmt));
    break;
  case Stmt::Kind::Return:
    sema_.returnChecker().checkReturn(cast<ReturnStmt>(stmt));
    sema_.flow_.unreachable = true;
    break;
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue:
    sema_.jumpChecker().checkJump(stmt);
    sema_.flow_.unreachable = true;
    break;
  case Stmt::Kind::Throw:
    sema_.throwChecker().checkThrow(cast<ThrowStmt>(stmt));
    sema_.flow_.unreachable = true;
    break;
  case Stmt::Kind::Try:
    ++sema_.branchDepth_;
    sema_.throwChecker().checkTry(cast<TryStmt>(stmt));
    --sema_.branchDepth_;
    break;
  case Stmt::Kind::Empty:
  case Stmt::Kind::Debugger:
    break;
  }
}

void StmtChecker::checkIf(IfStmt *stmt) {
  Sema::Flow whenTrue, whenFalse;
  stmt->cond = sema_.truthChecker().checkCondition(stmt->cond, whenTrue, whenFalse);
  // In an instance, `if (typeof x == "string")` may be decided by the type
  // of x: the branch that cannot run is dropped, unchecked.
  if (std::optional<bool> constant =
          sema_.checkingInstance_ ? sema_.genericChecker().constantCondition(stmt->cond) : std::nullopt) {
    auto *lit = sema_.ctx_.make<BoolLitExpr>(stmt->cond->loc(), *constant);
    lit->type = Type::boolTy();
    stmt->cond = lit;
    if (*constant) {
      sema_.flow_ = whenTrue;
      checkBody(stmt->thenStmt);
      stmt->elseStmt = nullptr;
    } else {
      stmt->thenStmt = sema_.ctx_.make<EmptyStmt>(stmt->thenStmt->loc());
      sema_.flow_ = whenFalse;
      if (stmt->elseStmt) {
        checkBody(stmt->elseStmt);
      }
    }
    return;
  }
  sema_.flow_ = whenTrue;
  checkBody(stmt->thenStmt);
  Sema::Flow afterThen = sema_.flow_;
  sema_.flow_ = whenFalse;
  if (stmt->elseStmt) {
    checkBody(stmt->elseStmt);
  }
  sema_.flow_ = FlowChecker::join(afterThen, sema_.flow_);
}

// The body of `if`, `while` or `for`. As in TypeScript, a declaration there
// needs braces: it would be visible nowhere else.
void StmtChecker::checkBody(Stmt *body) {
  if (isa<VarDeclStmt>(body) || isa<TypeAliasDecl>(body) || isa<EnumDecl>(body) ||
      isa<FunctionDecl>(body)) {
    sema_.diags_.error(body->loc(), "a declaration cannot be the whole body here; wrap it in '{ }'");
    return;
  }
  checkStmt(body);
}

} // namespace tz
