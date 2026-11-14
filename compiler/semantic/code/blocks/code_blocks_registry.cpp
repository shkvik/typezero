#include "compiler/semantic/code/blocks/code_blocks_registry.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

CodeBlocksRegistry::CodeBlocksRegistry(SemanticState &owner)
    : state(owner) {}

void CodeBlocksRegistry::walkStmt(const WalkStmt &task) {
  Stmt *stmt = task.stmt;
  auto &tasks = this->state.walk.tasks;
  switch (stmt->kind()) {
    case Stmt::Kind::Block:
      return this->startBlock(cast<BlockStmt>(stmt));
    case Stmt::Kind::VarDecl:
      return tasks.pushTask(WalkVarDecl{.varDecl = cast<VarDeclStmt>(stmt)});
    case Stmt::Kind::TypeAlias:
    case Stmt::Kind::Enum:
      return;
    case Stmt::Kind::Class:
      return tasks.pushTask(CheckClassStmt{.classDecl = cast<ClassDecl>(stmt)});
    case Stmt::Kind::Function:
      return this->walkFunctionStmt(cast<FunctionDecl>(stmt));
    case Stmt::Kind::Expr:
      return this->startExprStmt(cast<ExprStmt>(stmt));
    case Stmt::Kind::If:
      this->openBranch();
      return tasks.pushTask(WalkIf{.ifStmt = cast<IfStmt>(stmt)});
    case Stmt::Kind::While:
      this->openBranch();
      return tasks.pushTask(WalkWhile{.whileStmt = cast<WhileStmt>(stmt)});
    case Stmt::Kind::DoWhile:
      this->openBranch();
      return tasks.pushTask(WalkDoWhile{.doWhile = cast<DoWhileStmt>(stmt)});
    case Stmt::Kind::For:
      this->openBranch();
      return tasks.pushTask(WalkFor{.forStmt = cast<ForStmt>(stmt)});
    case Stmt::Kind::ForOf:
      this->openBranch();
      return tasks.pushTask(WalkForOf{.forOf = cast<ForOfStmt>(stmt)});
    case Stmt::Kind::Switch:
      this->openBranch();
      return tasks.pushTask(WalkSwitch{.switchStmt = cast<SwitchStmt>(stmt)});
    case Stmt::Kind::Labeled:
      return tasks.pushTask(WalkLabeled{.labeled = cast<LabeledStmt>(stmt)});
    case Stmt::Kind::Return:
      tasks.pushTask(MarkUnreachable{});
      return tasks.pushTask(WalkReturn{.returnStmt = cast<ReturnStmt>(stmt)});
    case Stmt::Kind::Break:
    case Stmt::Kind::Continue:
      tasks.pushTask(MarkUnreachable{});
      return tasks.pushTask(CheckJump{.stmt = stmt});
    case Stmt::Kind::Empty:
    case Stmt::Kind::Debugger:
      return;
  }
}

void CodeBlocksRegistry::openBranch() {
  this->state.walk.scopes.enterBranch();

  return this->state.walk.tasks.pushTask(LeaveBranch{});
}

void CodeBlocksRegistry::leaveBranch() {
  return this->state.walk.scopes.leaveBranch();
}

void CodeBlocksRegistry::markUnreachable() {
  return this->state.walk.flow.markUnreachable();
}

void CodeBlocksRegistry::startBlock(BlockStmt *block) {
  this->state.walk.scopes.openScope();
  this->state.walk.tasks.pushTask(CloseScope{});

  return this->state.walk.tasks.pushTask(WalkStatements{.block = block});
}

void CodeBlocksRegistry::startExprStmt(ExprStmt *statement) {
  auto *yield = dyn_cast<YieldExpr>(statement->expr);
  if (yield != nullptr) {
    yield->discarded = true;
  }
  this->state.walk.tasks.pushTask(AfterExprStmt{.statement = statement});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &statement->expr, .expected = nullptr});
}

void CodeBlocksRegistry::afterExprStmt(const AfterExprStmt &task) {
  this->state.walk.tasks.pushTask(FinishExprStmt{.statement = task.statement});

  return this->state.walk.tasks.pushTask(CheckUsedValue{.expr = task.statement->expr});
}

void CodeBlocksRegistry::finishExprStmt(const FinishExprStmt &task) {
  ExprStmt *statement = task.statement;
  auto *made = dyn_cast<NewExpr>(statement->expr);
  if (made != nullptr && made->cls() != nullptr) {
    this->state.library.events.addDroppedNew(made->cls(), made->loc());
  }
  if (this->state.flow.reachability.isExit(statement)) {
    this->state.walk.flow.markUnreachable();
  }
}

void CodeBlocksRegistry::walkBody(const WalkBody &task) {
  auto statement = this->checkBodyStatement(task.body);
  if (statement.isErr()) {
    return this->state.diagnostics.error(task.body->loc(), statement.error().message);
  }

  return this->walkStmt(WalkStmt{.stmt = task.body});
}

CodeBlocksRegistry::ResultBodyIsStatement
CodeBlocksRegistry::checkBodyStatement(const Stmt *body) const {
  bool isDeclaration = isa<VarDeclStmt>(body) || isa<TypeAliasDecl>(body) || isa<EnumDecl>(body) ||
                       isa<FunctionDecl>(body);
  if (isDeclaration) {
    auto message = ErrorTextBody::declared();

    return err(CodeBlocksRegistryCheckBodyError{
        .code = CodeBlocksRegistryCheckBodyErrorCode::DECLARATION_AS_BODY,
        .message = message,
    });
  }

  return ok(body);
}

void CodeBlocksRegistry::walkFunctionStmt(FunctionDecl *functionDecl) {
  auto placed = this->checkFunctionStmt(functionDecl);
  if (placed.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), placed.error().message);
  }
}

CodeBlocksRegistry::ResultFunctionInBlock
CodeBlocksRegistry::checkFunctionStmt(FunctionDecl *functionDecl) const {
  if (functionDecl->group == nullptr) {
    auto message = ErrorTextFunctionStmt::nested();

    return err(CodeBlocksRegistryCheckFunctionStmtError{
        .code = CodeBlocksRegistryCheckFunctionStmtErrorCode::NOT_IN_BLOCK,
        .message = message,
    });
  }

  return ok(functionDecl);
}

}
