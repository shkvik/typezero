#include "compiler/pipeline/check_program/topics/stmt/stmt_checker/stmt_checker.h"
#include "compiler/pipeline/check_program/flow/flow_checker/flow_checker.h"
#include "compiler/pipeline/check_program/flow/predicate_checker/predicate_checker.h"
#include "compiler/pipeline/check_program/generics/generic_checker/generic_checker.h"
#include "compiler/pipeline/check_program/scopes/scopes.h"
#include "compiler/pipeline/check_program/topics/classes/class_checker/class_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/expr/truth_checker/truth_checker.h"
#include "compiler/pipeline/check_program/topics/functions/closure_checker/closure_checker.h"
#include "compiler/pipeline/check_program/topics/result/result_checker/result_checker.h"
#include "compiler/pipeline/check_program/topics/stmt/jump_checker/jump_checker.h"
#include "compiler/pipeline/check_program/topics/stmt/loop_checker/loop_checker.h"
#include "compiler/pipeline/check_program/topics/stmt/return_checker/return_checker.h"
#include "compiler/pipeline/check_program/topics/stmt/switch_checker/switch_checker.h"
#include "compiler/pipeline/check_program/topics/stmt/var_checker/var_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

StmtChecker::StmtChecker(CheckState &owner)
    : state(owner) {}

void StmtChecker::checkStmt(Stmt *stmt) {
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    this->state.getScopeStack().openScope();
    ClosureChecker(this->state).checkStatements(cast<BlockStmt>(stmt));
    Scopes(this->state).closeScope();
    break;
  case Stmt::Kind::VarDecl:
    VarChecker(this->state).checkVarDecl(cast<VarDeclStmt>(stmt));
    break;
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
    break;
  case Stmt::Kind::Class: {
    auto *cls = cast<ClassDecl>(stmt);
    ClassChecker::ResultClassDeclared declared = ClassChecker(this->state).checkClassStmt(cls);
    if (declared.isErr()) {
      this->state.getDiagnostics().error(cls->loc(), declared.error().message);
    }
    break;
  }
  case Stmt::Kind::Function: {
    ResultFunctionInBlock placed = this->checkFunctionStmt(cast<FunctionDecl>(stmt));
    if (placed.isErr()) {
      this->state.getDiagnostics().error(stmt->loc(), placed.error().message);
    }
    break;
  }
  case Stmt::Kind::Expr: {
    auto *s = cast<ExprStmt>(stmt);
    if (auto *yield = dyn_cast<YieldExpr>(s->expr)) {
      yield->discarded = true;
    }
    s->expr = ExprChecker(this->state).checkExpr(s->expr);
    ResultChecker(this->state).checkDiscarded(s->expr, false, s->loc());
    if (auto *made = dyn_cast<NewExpr>(s->expr); made && made->cls()) {
      this->state.getEventStore().addDroppedNew(made->cls(), made->loc());
    }
    if (ReturnChecker::isExit(s)) {
      this->state.getFlowState().markUnreachable();
    }
    break;
  }
  case Stmt::Kind::If:
    this->state.getScopeStack().enterBranch();
    this->checkIf(cast<IfStmt>(stmt));
    this->state.getScopeStack().leaveBranch();
    break;
  case Stmt::Kind::While:
    this->state.getScopeStack().enterBranch();
    LoopChecker(this->state).checkWhile(cast<WhileStmt>(stmt));
    this->state.getScopeStack().leaveBranch();
    break;
  case Stmt::Kind::DoWhile:
    this->state.getScopeStack().enterBranch();
    LoopChecker(this->state).checkDoWhile(cast<DoWhileStmt>(stmt));
    this->state.getScopeStack().leaveBranch();
    break;
  case Stmt::Kind::For:
    this->state.getScopeStack().enterBranch();
    LoopChecker(this->state).checkFor(cast<ForStmt>(stmt));
    this->state.getScopeStack().leaveBranch();
    break;
  case Stmt::Kind::ForOf:
    this->state.getScopeStack().enterBranch();
    LoopChecker(this->state).checkForOf(cast<ForOfStmt>(stmt));
    this->state.getScopeStack().leaveBranch();
    break;
  case Stmt::Kind::Switch:
    this->state.getScopeStack().enterBranch();
    SwitchChecker(this->state).checkSwitch(cast<SwitchStmt>(stmt));
    this->state.getScopeStack().leaveBranch();
    break;
  case Stmt::Kind::Labeled:
    JumpChecker(this->state).checkLabeled(cast<LabeledStmt>(stmt));
    break;
  case Stmt::Kind::Return:
    ReturnChecker(this->state).checkReturn(cast<ReturnStmt>(stmt));
    this->state.getFlowState().markUnreachable();
    break;
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue: {
    JumpChecker::ResultJumpTargeted jumped = JumpChecker(this->state).checkJump(stmt);
    if (jumped.isErr()) {
      this->state.getDiagnostics().error(stmt->loc(), jumped.error().message);
    }
    this->state.getFlowState().markUnreachable();
    break;
  }
  case Stmt::Kind::Empty:
  case Stmt::Kind::Debugger:
    break;
  }
}

void StmtChecker::checkIf(IfStmt *stmt) {
  Flow whenTrue, whenFalse;
  stmt->cond = TruthChecker(this->state).checkCondition(stmt->cond, whenTrue, whenFalse);
  if (std::optional<bool> constant = this->state.getGenericStore().isCheckingInstance()
                                         ? GenericChecker(this->state).constantCondition(stmt->cond)
                                         : std::nullopt) {
    auto *lit = this->state.getAst().make<BoolLitExpr>(stmt->cond->loc(), *constant);
    lit->type = Type::boolTy();
    stmt->cond = lit;
    if (*constant) {
      this->state.getFlowState().replaceFlow(whenTrue);
      ResultBodyIsStatement thenBody = this->checkBody(stmt->thenStmt);
      if (thenBody.isErr()) {
        this->state.getDiagnostics().error(stmt->thenStmt->loc(), thenBody.error().message);
      }
      stmt->elseStmt = nullptr;
    } else {
      stmt->thenStmt = this->state.getAst().make<EmptyStmt>(stmt->thenStmt->loc());
      this->state.getFlowState().replaceFlow(whenFalse);
      if (stmt->elseStmt) {
        ResultBodyIsStatement elseBody = this->checkBody(stmt->elseStmt);
        if (elseBody.isErr()) {
          this->state.getDiagnostics().error(stmt->elseStmt->loc(), elseBody.error().message);
        }
      }
    }
    return;
  }
  this->state.getFlowState().replaceFlow(whenTrue);
  ResultBodyIsStatement thenBody = this->checkBody(stmt->thenStmt);
  if (thenBody.isErr()) {
    this->state.getDiagnostics().error(stmt->thenStmt->loc(), thenBody.error().message);
  }
  Flow afterThen = this->state.getFlowState().getFlow();
  this->state.getFlowState().replaceFlow(whenFalse);
  if (stmt->elseStmt) {
    ResultBodyIsStatement elseBody = this->checkBody(stmt->elseStmt);
    if (elseBody.isErr()) {
      this->state.getDiagnostics().error(stmt->elseStmt->loc(), elseBody.error().message);
    }
  }
  this->state.getFlowState().replaceFlow(
      FlowChecker::join(afterThen, this->state.getFlowState().getFlow()));
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
