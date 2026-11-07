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
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

StmtChecker::StmtChecker(Checker &owner)
    : checker(owner) {}

void StmtChecker::checkStmt(Stmt *stmt) {
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    this->checker.pushScope();
    this->checker.closureChecker().checkStatements(cast<BlockStmt>(stmt));
    this->checker.popScope();
    break;
  case Stmt::Kind::VarDecl:
    this->checker.varChecker().checkVarDecl(cast<VarDeclStmt>(stmt));
    break;
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
    break;
  case Stmt::Kind::Class: {
    auto *cls = cast<ClassDecl>(stmt);
    ClassChecker::ResultClassDeclared declared = this->checker.classChecker().checkClassStmt(cls);
    if (declared.isErr()) {
      this->checker.diags_.error(cls->loc(), declared.error().message);
    }
    break;
  }
  case Stmt::Kind::Function: {
    ResultFunctionInBlock placed = this->checkFunctionStmt(cast<FunctionDecl>(stmt));
    if (placed.isErr()) {
      this->checker.diags_.error(stmt->loc(), placed.error().message);
    }
    break;
  }
  case Stmt::Kind::Expr: {
    auto *s = cast<ExprStmt>(stmt);
    if (auto *yield = dyn_cast<YieldExpr>(s->expr)) {
      yield->discarded = true;
    }
    s->expr = this->checker.exprChecker().checkExpr(s->expr);
    this->checker.resultChecker().checkDiscarded(s->expr, false, s->loc());
    if (auto *made = dyn_cast<NewExpr>(s->expr); made && made->cls()) {
      this->checker.droppedNews_.push_back({made->cls(), made->loc()});
    }
    if (ReturnChecker::isExit(s)) {
      this->checker.flow_.unreachable = true;
    }
    break;
  }
  case Stmt::Kind::If:
    ++this->checker.branchDepth_;
    this->checkIf(cast<IfStmt>(stmt));
    --this->checker.branchDepth_;
    break;
  case Stmt::Kind::While:
    ++this->checker.branchDepth_;
    this->checker.loopChecker().checkWhile(cast<WhileStmt>(stmt));
    --this->checker.branchDepth_;
    break;
  case Stmt::Kind::DoWhile:
    ++this->checker.branchDepth_;
    this->checker.loopChecker().checkDoWhile(cast<DoWhileStmt>(stmt));
    --this->checker.branchDepth_;
    break;
  case Stmt::Kind::For:
    ++this->checker.branchDepth_;
    this->checker.loopChecker().checkFor(cast<ForStmt>(stmt));
    --this->checker.branchDepth_;
    break;
  case Stmt::Kind::ForOf:
    ++this->checker.branchDepth_;
    this->checker.loopChecker().checkForOf(cast<ForOfStmt>(stmt));
    --this->checker.branchDepth_;
    break;
  case Stmt::Kind::Switch:
    ++this->checker.branchDepth_;
    this->checker.switchChecker().checkSwitch(cast<SwitchStmt>(stmt));
    --this->checker.branchDepth_;
    break;
  case Stmt::Kind::Labeled:
    this->checker.jumpChecker().checkLabeled(cast<LabeledStmt>(stmt));
    break;
  case Stmt::Kind::Return:
    this->checker.returnChecker().checkReturn(cast<ReturnStmt>(stmt));
    this->checker.flow_.unreachable = true;
    break;
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue: {
    JumpChecker::ResultJumpTargeted jumped = this->checker.jumpChecker().checkJump(stmt);
    if (jumped.isErr()) {
      this->checker.diags_.error(stmt->loc(), jumped.error().message);
    }
    this->checker.flow_.unreachable = true;
    break;
  }
  case Stmt::Kind::Empty:
  case Stmt::Kind::Debugger:
    break;
  }
}

void StmtChecker::checkIf(IfStmt *stmt) {
  Checker::Flow whenTrue, whenFalse;
  stmt->cond = this->checker.truthChecker().checkCondition(stmt->cond, whenTrue, whenFalse);
  if (std::optional<bool> constant =
          this->checker.checkingInstance_
              ? this->checker.genericChecker().constantCondition(stmt->cond)
              : std::nullopt) {
    auto *lit = this->checker.ctx_.make<BoolLitExpr>(stmt->cond->loc(), *constant);
    lit->type = Type::boolTy();
    stmt->cond = lit;
    if (*constant) {
      this->checker.flow_ = whenTrue;
      ResultBodyIsStatement thenBody = this->checkBody(stmt->thenStmt);
      if (thenBody.isErr()) {
        this->checker.diags_.error(stmt->thenStmt->loc(), thenBody.error().message);
      }
      stmt->elseStmt = nullptr;
    } else {
      stmt->thenStmt = this->checker.ctx_.make<EmptyStmt>(stmt->thenStmt->loc());
      this->checker.flow_ = whenFalse;
      if (stmt->elseStmt) {
        ResultBodyIsStatement elseBody = this->checkBody(stmt->elseStmt);
        if (elseBody.isErr()) {
          this->checker.diags_.error(stmt->elseStmt->loc(), elseBody.error().message);
        }
      }
    }
    return;
  }
  this->checker.flow_ = whenTrue;
  ResultBodyIsStatement thenBody = this->checkBody(stmt->thenStmt);
  if (thenBody.isErr()) {
    this->checker.diags_.error(stmt->thenStmt->loc(), thenBody.error().message);
  }
  Checker::Flow afterThen = this->checker.flow_;
  this->checker.flow_ = whenFalse;
  if (stmt->elseStmt) {
    ResultBodyIsStatement elseBody = this->checkBody(stmt->elseStmt);
    if (elseBody.isErr()) {
      this->checker.diags_.error(stmt->elseStmt->loc(), elseBody.error().message);
    }
  }
  this->checker.flow_ = FlowChecker::join(afterThen, this->checker.flow_);
}

StmtChecker::ResultFunctionInBlock StmtChecker::checkFunctionStmt(FunctionDecl *fn) const {
  if (!fn->group) {
    std::string message = ErrorTextFunctionStmt::nested();

    return err(StmtCheckerCheckFunctionStmtError{
        .code = StmtCheckerCheckFunctionStmtErrorCode::NOT_IN_BLOCK,
        .message = message,
    });
  }

  return ok(fn);
}

StmtChecker::ResultBodyIsStatement StmtChecker::checkBody(Stmt *body) {
  bool declaration = isa<VarDeclStmt>(body) || isa<TypeAliasDecl>(body) || isa<EnumDecl>(body) ||
                     isa<FunctionDecl>(body);
  if (declaration) {
    std::string message = ErrorTextBody::declared();

    return err(StmtCheckerCheckBodyError{
        .code = StmtCheckerCheckBodyErrorCode::DECLARATION_AS_BODY,
        .message = message,
    });
  }

  this->checkStmt(body);

  return ok(body);
}

}
