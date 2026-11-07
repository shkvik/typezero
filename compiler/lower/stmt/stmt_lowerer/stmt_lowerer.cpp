#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/debug_info/debug_info.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/functions/tail_call_lowerer/tail_call_lowerer.h"
#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/memory/publish_lowerer/publish_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/exit_lowerer/exit_lowerer.h"
#include "compiler/lower/stmt/loop_lowerer/loop_lowerer.h"
#include "compiler/lower/stmt/switch_lowerer/switch_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

StmtLowerer::StmtLowerer(Lowering &owner)
    : lowering(owner) {}

void StmtLowerer::emitStmt(Stmt *stmt) {
  this->lowering.orderAt(stmt->loc());
  llvm::DebugLoc outer = this->lowering.debugInfo().here();
  this->lowering.debugInfo().at(stmt->loc());
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    this->lowering.functionFrame().pushScope();
    this->emitStatements(cast<BlockStmt>(stmt));
    this->lowering.debugInfo().at(cast<BlockStmt>(stmt)->end);
    this->lowering.functionFrame().popScope();
    return;
  case Stmt::Kind::VarDecl:
    this->emitVarDecl(cast<VarDeclStmt>(stmt));
    break;
  case Stmt::Kind::Function:
    break;
  case Stmt::Kind::Expr:
    if (this->lowering.tailStatements_.contains(stmt)) {
      this->lowering.tailCallLowerer().emitTailJump(cast<CallExpr>(cast<ExprStmt>(stmt)->expr));
    } else {
      this->emitDiscarded(cast<ExprStmt>(stmt)->expr);
    }
    break;
  case Stmt::Kind::If:
    this->emitIf(cast<IfStmt>(stmt));
    break;
  case Stmt::Kind::While:
    this->lowering.loopLowerer().emitWhile(cast<WhileStmt>(stmt));
    break;
  case Stmt::Kind::DoWhile:
    this->lowering.loopLowerer().emitDoWhile(cast<DoWhileStmt>(stmt));
    break;
  case Stmt::Kind::For:
    this->lowering.loopLowerer().emitFor(cast<ForStmt>(stmt));
    break;
  case Stmt::Kind::ForOf:
    this->lowering.loopLowerer().emitForOf(cast<ForOfStmt>(stmt));
    break;
  case Stmt::Kind::Switch:
    this->lowering.switchLowerer().emitSwitch(cast<SwitchStmt>(stmt));
    break;
  case Stmt::Kind::Labeled:
    this->lowering.loopLowerer().emitLabeled(cast<LabeledStmt>(stmt));
    break;
  case Stmt::Kind::Return:
    this->emitReturn(cast<ReturnStmt>(stmt));
    break;
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue:
    this->lowering.loopLowerer().emitJump(stmt);
    break;
  case Stmt::Kind::Empty:
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
    break;
  case Stmt::Kind::Class:
    if (!cast<ClassDecl>(stmt)->expression()) {
      this->lowering.decoratorLowerer().emitClassDeclaration(cast<ClassDecl>(stmt));
    }
    break;
  case Stmt::Kind::Debugger:
    this->lowering.builder_.CreateCall(this->lowering.rt_.debugger);
    break;
  }
  this->lowering.debugInfo().restore(outer);
}

void StmtLowerer::emitStatements(BlockStmt *block) {
  NestedGroup *group = block->group;
  for (size_t i = 0; i < block->body.size(); ++i) {
    if (group && i == group->firstUse) {
      this->lowering.envLowerer().emitGroupCreation(group);
    }
    if (i + 1 < block->body.size()) {
      this->lowering.noteAwaitedNext(block->body[i], block->body[i + 1]);
    }
    this->emitStmt(block->body[i]);
  }
}

void StmtLowerer::storeReplacing(llvm::Value *address, llvm::Value *value, const Type *type) {
  if (!type->isManaged()) {
    this->lowering.builder_.CreateStore(value, address);
    return;
  }
  llvm::Value *previous =
      this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(type), address);
  this->lowering.builder_.CreateStore(value, address);
  this->lowering.refCounts().releaseValue(previous, type);
}

void StmtLowerer::emitVarDecl(VarDeclStmt *decl) {
  for (VarDeclarator &d : decl->decls) {
    if (d.alias) {
      continue;
    }
    const VarSymbol *var = d.sym;
    llvm::Type *type = this->lowering.moduleBuilder().lower(var->type);
    bool global = var->isGlobal() && var->usedInFunction();
    llvm::Value *address = global ? this->lowering.functionFrame().addressOf(var)
                                  : this->lowering.functionFrame().createLocal(var, d.name);
    llvm::Value *value =
        d.init ? this->lowering.exprLowerer().emitExpr(d.init) : llvm::Constant::getNullValue(type);
    this->lowering.storeLowerer().storeVar(var, address, value, true);
    if (this->lowering.isEternal(var)) {
      this->lowering.publishLowerer().pinValue(value, var->type);
      if (!global) {
        this->lowering.eternalSlots_.insert(address);
      }
    }
  }
}

void StmtLowerer::emitIf(IfStmt *stmt) {
  llvm::Value *cond = this->lowering.exprLowerer().emitExpr(stmt->cond);
  llvm::BasicBlock *thenBlock = this->lowering.functionFrame().newBlock("if.then");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("if.end");
  llvm::BasicBlock *elseBlock =
      stmt->elseStmt ? this->lowering.functionFrame().newBlock("if.else") : endBlock;
  this->lowering.builder_.CreateCondBr(cond, thenBlock, elseBlock);

  this->lowering.functionFrame().switchTo(thenBlock);
  this->emitStmt(stmt->thenStmt);
  this->lowering.functionFrame().branchTo(endBlock);

  if (stmt->elseStmt) {
    this->lowering.functionFrame().switchTo(elseBlock);
    this->emitStmt(stmt->elseStmt);
    this->lowering.functionFrame().branchTo(endBlock);
  }
  this->lowering.functionFrame().switchTo(endBlock);
}

void StmtLowerer::emitReturn(ReturnStmt *stmt) {
  if (this->lowering.inline_) {
    llvm::Value *value = stmt->value ? this->lowering.exprLowerer().emitExpr(stmt->value) : nullptr;
    if (value && this->lowering.inline_->returnSlot) {
      this->storeReplacing(this->lowering.inline_->returnSlot, value, stmt->value->type);
    } else if (value) {
      this->lowering.refCounts().releaseValue(value, stmt->value->type);
    }
    this->lowering.functionFrame().emitCleanups(this->lowering.inline_->scopeDepth);
    this->lowering.builder_.CreateBr(this->lowering.inline_->exit);
    this->lowering.functionFrame().startDeadBlock();
    return;
  }
  if (this->lowering.coro_ && this->lowering.currentFn_ && this->lowering.currentFn_->isGenerator &&
      this->lowering.async_) {
    this->lowering.asyncGeneratorLowerer().emitAsyncGeneratorReturn(stmt);
    return;
  }
  if (this->lowering.coro_ && this->lowering.currentFn_ && this->lowering.currentFn_->isGenerator) {
    this->lowering.exitLowerer().emitReturnOf(
        stmt->value ? this->lowering.exprLowerer().emitExpr(stmt->value) : nullptr);
    return;
  }
  if (this->lowering.async_) {
    this->lowering.asyncLowerer().emitAsyncReturnStmt(stmt);
    return;
  }
  if (stmt->value && this->lowering.tailGroup_ &&
      this->lowering.tailCallLowerer().hasTailCall(stmt->value)) {
    this->lowering.tailCallLowerer().emitTailReturn(stmt->value);
    return;
  }
  this->emitReturnValue(stmt->value);
}

void StmtLowerer::emitReturnValue(Expr *expr) {
  llvm::Value *value = expr ? this->lowering.exprLowerer().emitExpr(expr) : nullptr;
  if (value && this->lowering.returnType_ && this->lowering.returnType_->isVoid() &&
      !expr->type->isVoid()) {
    this->lowering.refCounts().releaseValue(value, expr->type);
    value = nullptr;
  }
  this->lowering.exitLowerer().emitReturnOf(value && !expr->type->isVoid() ? value : nullptr);
}

void StmtLowerer::emitDiscarded(Expr *expr) {
  auto *assign = dyn_cast<AssignExpr>(expr);
  if (assign) {
    this->lowering.assignLowerer().emitAssign(assign, false);
    return;
  }
  auto *seq = dyn_cast<SequenceExpr>(expr);
  if (seq) {
    for (Expr *e : seq->exprs) {
      this->emitDiscarded(e);
    }
    return;
  }
  llvm::Value *value = this->lowering.exprLowerer().emitExpr(expr);
  if (expr->type->isManaged()) {
    this->lowering.refCounts().releaseValue(value, expr->type);
  }
}

}
