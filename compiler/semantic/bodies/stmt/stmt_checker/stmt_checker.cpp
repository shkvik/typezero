#include "compiler/semantic/bodies/stmt/stmt_checker/stmt_checker.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/truth_checker/truth_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/bodies/stmt/jump_checker/jump_checker.h"
#include "compiler/semantic/bodies/stmt/loop_checker/loop_checker.h"
#include "compiler/semantic/bodies/stmt/return_checker/return_checker.h"
#include "compiler/semantic/bodies/stmt/switch_checker/switch_checker.h"
#include "compiler/semantic/bodies/stmt/var_checker/var_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

StmtChecker::StmtChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->varChecker = &all.varChecker;
  this->exprChecker = &all.exprChecker;
  this->jumpChecker = &all.jumpChecker;
  this->loopChecker = &all.loopChecker;
  this->classChecker = &all.classChecker;
  this->truthChecker = &all.truthChecker;
  this->resultChecker = &all.resultChecker;
  this->returnChecker = &all.returnChecker;
  this->switchChecker = &all.switchChecker;
  this->closureChecker = &all.closureChecker;
  this->genericChecker = &all.genericChecker;
}

void StmtChecker::walkStmt(Stmt *stmt) {
  switch (stmt->kind()) {
    case Stmt::Kind::Block:
      return this->walkBlock(cast<BlockStmt>(stmt));
    case Stmt::Kind::VarDecl:
      return this->varChecker->walkVarDecl(cast<VarDeclStmt>(stmt));
    case Stmt::Kind::TypeAlias:
    case Stmt::Kind::Enum:
      return;
    case Stmt::Kind::Class:
      return this->walkClassStmt(cast<ClassDecl>(stmt));
    case Stmt::Kind::Function:
      return this->walkFunctionStmt(cast<FunctionDecl>(stmt));
    case Stmt::Kind::Expr:
      return this->walkExprStmt(cast<ExprStmt>(stmt));
    case Stmt::Kind::If:
      this->state.getScopeStack().enterBranch();
      this->walkIf(cast<IfStmt>(stmt));
      return this->state.getScopeStack().leaveBranch();
    case Stmt::Kind::While:
      this->state.getScopeStack().enterBranch();
      this->loopChecker->walkWhile(cast<WhileStmt>(stmt));
      return this->state.getScopeStack().leaveBranch();
    case Stmt::Kind::DoWhile:
      this->state.getScopeStack().enterBranch();
      this->loopChecker->walkDoWhile(cast<DoWhileStmt>(stmt));
      return this->state.getScopeStack().leaveBranch();
    case Stmt::Kind::For:
      this->state.getScopeStack().enterBranch();
      this->loopChecker->walkFor(cast<ForStmt>(stmt));
      return this->state.getScopeStack().leaveBranch();
    case Stmt::Kind::ForOf:
      this->state.getScopeStack().enterBranch();
      this->loopChecker->walkForOf(cast<ForOfStmt>(stmt));
      return this->state.getScopeStack().leaveBranch();
    case Stmt::Kind::Switch:
      this->state.getScopeStack().enterBranch();
      this->switchChecker->walkSwitch(cast<SwitchStmt>(stmt));
      return this->state.getScopeStack().leaveBranch();
    case Stmt::Kind::Labeled:
      return this->jumpChecker->walkLabeled(cast<LabeledStmt>(stmt));
    case Stmt::Kind::Return:
      this->returnChecker->walkReturn(cast<ReturnStmt>(stmt));
      return this->state.getFlowState().markUnreachable();
    case Stmt::Kind::Break:
    case Stmt::Kind::Continue:
      return this->walkJump(stmt);
    case Stmt::Kind::Empty:
    case Stmt::Kind::Debugger:
      return;
  }
}

void StmtChecker::walkBlock(BlockStmt *block) {
  this->state.getScopeStack().openScope();
  this->closureChecker->walkStatements(block);
  this->scopes->closeScope();
}

void StmtChecker::walkClassStmt(ClassDecl *cls) {
  ClassChecker::ResultClassDeclared declared = this->classChecker->checkClassStmt(cls);
  if (declared.isErr()) {
    this->state.getDiagnostics().error(cls->loc(), declared.error().message);
  }
}

void StmtChecker::walkFunctionStmt(FunctionDecl *fn) {
  ResultFunctionInBlock placed = this->checkFunctionStmt(fn);
  if (placed.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), placed.error().message);
  }
}

void StmtChecker::walkExprStmt(ExprStmt *s) {
  auto *yield = dyn_cast<YieldExpr>(s->expr);
  if (yield != nullptr) {
    yield->discarded = true;
  }
  s->expr = this->exprChecker->checkExpr(s->expr);
  ResultChecker::ResultValueGetsUsed used =
      this->resultChecker->usedValue(s->expr, false, s->loc());
  if (used.isErr()) {
    this->state.getDiagnostics().error(s->expr->loc(), used.error().message);
  }
  auto *made = dyn_cast<NewExpr>(s->expr);
  if (made != nullptr && made->cls() != nullptr) {
    this->state.getEventStore().addDroppedNew(made->cls(), made->loc());
  }
  if (ReturnChecker::isExit(s)) {
    this->state.getFlowState().markUnreachable();
  }
}

void StmtChecker::walkJump(Stmt *stmt) {
  JumpChecker::ResultJumpTargeted jumped = this->jumpChecker->checkJump(stmt);
  if (jumped.isErr()) {
    this->state.getDiagnostics().error(stmt->loc(), jumped.error().message);
  }
  this->state.getFlowState().markUnreachable();
}

void StmtChecker::walkIf(IfStmt *stmt) {
  Flow whenTrue, whenFalse;
  stmt->cond = this->truthChecker->checkCondition(stmt->cond, whenTrue, whenFalse);
  if (std::optional<bool> constant = this->state.getGenericStore().isCheckingInstance()
                                         ? this->genericChecker->constantCondition(stmt->cond)
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
  bool isDeclaration = isa<VarDeclStmt>(body) || isa<TypeAliasDecl>(body) || isa<EnumDecl>(body) ||
                       isa<FunctionDecl>(body);
  if (isDeclaration) {
    std::string message = ErrorTextBody::declared();

    return err(StmtCheckerCheckBodyError{
        .code = StmtCheckerCheckBodyErrorCode::DECLARATION_AS_BODY,
        .message = message,
    });
  }

  this->walkStmt(body);

  return ok(body);
}

}
