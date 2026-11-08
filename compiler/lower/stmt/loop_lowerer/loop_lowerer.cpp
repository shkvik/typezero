#include "compiler/lower/stmt/loop_lowerer/loop_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/exit_lowerer/exit_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

LoopLowerer::LoopLowerer(Lowering &owner)
    : lowering(owner) {}

void LoopLowerer::pushLoop(llvm::BasicBlock *breakTarget, llvm::BasicBlock *continueTarget,
                           size_t scopeDepth) {
  this->lowering.targets.push_back({std::move(this->lowering.pendingLabels), true, false,
                                    breakTarget, continueTarget, scopeDepth});
  this->lowering.pendingLabels.clear();
}

void LoopLowerer::emitJump(Stmt *stmt) {
  bool isBreak = isa<BreakStmt>(stmt);
  llvm::StringRef label = isBreak ? cast<BreakStmt>(stmt)->label : cast<ContinueStmt>(stmt)->label;
  for (size_t i = this->lowering.targets.size(); i > 0; --i) {
    const Lowering::JumpTarget &t = this->lowering.targets[i - 1];
    bool matches = !label.empty() ? llvm::is_contained(t.labels, label)
                   : isBreak      ? t.isLoop || t.isSwitch
                                  : t.isLoop;
    if (matches) {
      this->lowering.exitLowerer().emitJumpTo(i - 1, isBreak);
      return;
    }
  }
  llvm_unreachable("Checker checks every break and continue");
}

void LoopLowerer::emitLabeled(LabeledStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels;
  Stmt *body = stmt;
  while (auto *labeled = dyn_cast<LabeledStmt>(body)) {
    labels.push_back(labeled->label);
    body = labeled->body;
  }
  if (isa<WhileStmt>(body) || isa<DoWhileStmt>(body) || isa<ForStmt>(body) ||
      isa<ForOfStmt>(body) || isa<SwitchStmt>(body)) {
    this->lowering.pendingLabels = labels;
    this->lowering.stmtLowerer().emitStmt(body);
    return;
  }
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("label.end");
  this->lowering.targets.push_back(
      {labels, false, false, endBlock, nullptr, this->lowering.scopes.size()});
  this->lowering.stmtLowerer().emitStmt(body);
  this->lowering.targets.pop_back();
  this->lowering.functionFrame().emitBlock(endBlock);
}

void LoopLowerer::emitWhile(WhileStmt *stmt) {
  llvm::BasicBlock *condBlock = this->lowering.functionFrame().newBlock("while.cond");
  llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("while.body");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("while.end");

  this->lowering.functionFrame().emitBlock(condBlock);
  llvm::Value *cond = this->lowering.exprLowerer().emitExpr(stmt->cond);
  this->lowering.builder.CreateCondBr(cond, bodyBlock, endBlock);

  this->lowering.functionFrame().switchTo(bodyBlock);
  this->pushLoop(endBlock, condBlock, this->lowering.scopes.size());
  this->lowering.stmtLowerer().emitStmt(stmt->body);
  this->lowering.targets.pop_back();
  this->lowering.functionFrame().branchTo(condBlock);

  this->lowering.functionFrame().switchTo(endBlock);
}

void LoopLowerer::emitDoWhile(DoWhileStmt *stmt) {
  llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("do.body");
  llvm::BasicBlock *condBlock = this->lowering.functionFrame().newBlock("do.cond");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("do.end");

  this->lowering.functionFrame().emitBlock(bodyBlock);
  this->pushLoop(endBlock, condBlock, this->lowering.scopes.size());
  this->lowering.stmtLowerer().emitStmt(stmt->body);
  this->lowering.targets.pop_back();

  this->lowering.functionFrame().emitBlock(condBlock);
  llvm::Value *cond = this->lowering.exprLowerer().emitExpr(stmt->cond);
  this->lowering.builder.CreateCondBr(cond, bodyBlock, endBlock);

  this->lowering.functionFrame().switchTo(endBlock);
}

void LoopLowerer::emitFor(ForStmt *stmt) {
  this->lowering.functionFrame().pushScope();
  if (stmt->init) {
    this->lowering.stmtLowerer().emitStmt(stmt->init);
  }

  llvm::BasicBlock *condBlock = this->lowering.functionFrame().newBlock("for.cond");
  llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("for.body");
  llvm::BasicBlock *stepBlock = this->lowering.functionFrame().newBlock("for.step");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("for.end");

  this->lowering.functionFrame().emitBlock(condBlock);
  if (stmt->cond) {
    llvm::Value *cond = this->lowering.exprLowerer().emitExpr(stmt->cond);
    this->lowering.builder.CreateCondBr(cond, bodyBlock, endBlock);
  } else {
    this->lowering.builder.CreateBr(bodyBlock);
  }

  this->lowering.functionFrame().switchTo(bodyBlock);
  this->pushLoop(endBlock, stepBlock, this->lowering.scopes.size());
  this->lowering.stmtLowerer().emitStmt(stmt->body);
  this->lowering.targets.pop_back();

  this->lowering.functionFrame().emitBlock(stepBlock);
  auto *decl = llvm::dyn_cast_or_null<VarDeclStmt>(stmt->init);
  if (decl) {
    for (VarDeclarator &d : decl->decls) {
      if (d.alias || !d.sym->boxed()) {
        continue;
      }
      const Type *type = ClosureLowerer::typeOf(d.sym);
      llvm::Value *slot = this->lowering.boxSlots.lookup(d.sym);
      llvm::Value *old = this->lowering.builder.CreateLoad(this->lowering.builder.getPtrTy(), slot);
      llvm::Value *box = this->lowering.envLowerer().newBox(type);
      llvm::Value *value =
          this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(type),
                                            this->lowering.builder.CreateStructGEP(
                                                this->lowering.envLowerer().boxType(type), old, 2));
      this->lowering.refCounts().retainValue(value, type);
      this->lowering.builder.CreateStore(
          value, this->lowering.builder.CreateStructGEP(this->lowering.envLowerer().boxType(type),
                                                        box, 2));
      if (this->lowering.isCyclicBox(type)) {
        this->lowering.storeLowerer().groupStore(box, value, type);
      }
      this->lowering.builder.CreateStore(box, slot);
      this->lowering.builder.CreateCall(this->lowering.objReleaseFn, {old});
    }
  }
  if (stmt->step) {
    this->lowering.stmtLowerer().emitDiscarded(stmt->step);
  }
  this->lowering.builder.CreateBr(condBlock);

  this->lowering.functionFrame().switchTo(endBlock);
  this->lowering.functionFrame().popScope();
}

void LoopLowerer::emitForOf(ForOfStmt *stmt) {
  if (stmt->isAwait) {
    this->lowering.asyncGeneratorLowerer().emitForAwait(stmt);
    return;
  }
  if (stmt->iterable->type->isIterator() || stmt->iterable->type->isTypedArray()) {
    this->lowering.iteratorLowerer().emitIteratorForOf(stmt);
    return;
  }
  this->lowering.functionFrame().pushScope();
  const Type *iterableType = stmt->iterable->type;
  llvm::Value *iterableSlot =
      this->lowering.functionFrame().createManagedSlot(iterableType, "for.iterable");
  this->lowering.builder.CreateStore(this->lowering.exprLowerer().emitExpr(stmt->iterable),
                                     iterableSlot);

  llvm::BasicBlock *condBlock = this->lowering.functionFrame().newBlock("for.of.cond");
  llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("for.of.body");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("for.of.end");
  this->lowering.arrayLowerer().emitArrayForOf(stmt, iterableSlot, condBlock, bodyBlock, endBlock);
  this->lowering.functionFrame().switchTo(endBlock);
  this->lowering.functionFrame().popScope();
}

}
