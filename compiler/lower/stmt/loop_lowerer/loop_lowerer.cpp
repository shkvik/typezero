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
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/stmt/try_lowerer/try_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

LoopLowerer::LoopLowerer(Lowering &lowering) : lowering_(lowering) {}

void LoopLowerer::pushLoop(llvm::BasicBlock *breakTarget, llvm::BasicBlock *continueTarget,
                           size_t scopeDepth) {
  lowering_.targets_.push_back(
      {std::move(lowering_.pendingLabels_), true, false, breakTarget, continueTarget, scopeDepth});
  lowering_.pendingLabels_.clear();
}

void LoopLowerer::emitJump(Stmt *stmt) {
  bool isBreak = isa<BreakStmt>(stmt);
  llvm::StringRef label = isBreak ? cast<BreakStmt>(stmt)->label : cast<ContinueStmt>(stmt)->label;
  for (size_t i = lowering_.targets_.size(); i > 0; --i) {
    const Lowering::JumpTarget &t = lowering_.targets_[i - 1];
    bool matches = !label.empty() ? llvm::is_contained(t.labels, label)
                   : isBreak      ? t.isLoop || t.isSwitch
                                  : t.isLoop;
    if (matches) {
      lowering_.tryLowerer().emitJumpTo(i - 1, isBreak);
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
    lowering_.pendingLabels_ = labels;
    lowering_.stmtLowerer().emitStmt(body);
    return;
  }
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("label.end");
  lowering_.targets_.push_back({labels, false, false, endBlock, nullptr, lowering_.scopes_.size()});
  lowering_.stmtLowerer().emitStmt(body);
  lowering_.targets_.pop_back();
  lowering_.functionFrame().emitBlock(endBlock);
}

void LoopLowerer::emitWhile(WhileStmt *stmt) {
  llvm::BasicBlock *condBlock = lowering_.functionFrame().newBlock("while.cond");
  llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("while.body");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("while.end");

  lowering_.functionFrame().emitBlock(condBlock);
  llvm::Value *cond = lowering_.exprLowerer().emitExpr(stmt->cond);
  lowering_.builder_.CreateCondBr(cond, bodyBlock, endBlock);

  lowering_.functionFrame().switchTo(bodyBlock);
  pushLoop(endBlock, condBlock, lowering_.scopes_.size());
  lowering_.stmtLowerer().emitStmt(stmt->body);
  lowering_.targets_.pop_back();
  lowering_.functionFrame().branchTo(condBlock);

  lowering_.functionFrame().switchTo(endBlock);
}

void LoopLowerer::emitDoWhile(DoWhileStmt *stmt) {
  llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("do.body");
  llvm::BasicBlock *condBlock = lowering_.functionFrame().newBlock("do.cond");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("do.end");

  lowering_.functionFrame().emitBlock(bodyBlock);
  pushLoop(endBlock, condBlock, lowering_.scopes_.size());
  lowering_.stmtLowerer().emitStmt(stmt->body);
  lowering_.targets_.pop_back();

  lowering_.functionFrame().emitBlock(condBlock);
  llvm::Value *cond = lowering_.exprLowerer().emitExpr(stmt->cond);
  lowering_.builder_.CreateCondBr(cond, bodyBlock, endBlock);

  lowering_.functionFrame().switchTo(endBlock);
}

void LoopLowerer::emitFor(ForStmt *stmt) {
  lowering_.functionFrame().pushScope();
  if (stmt->init) {
    lowering_.stmtLowerer().emitStmt(stmt->init);
  }

  llvm::BasicBlock *condBlock = lowering_.functionFrame().newBlock("for.cond");
  llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("for.body");
  llvm::BasicBlock *stepBlock = lowering_.functionFrame().newBlock("for.step");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("for.end");

  lowering_.functionFrame().emitBlock(condBlock);
  if (stmt->cond) {
    llvm::Value *cond = lowering_.exprLowerer().emitExpr(stmt->cond);
    lowering_.builder_.CreateCondBr(cond, bodyBlock, endBlock);
  } else {
    lowering_.builder_.CreateBr(bodyBlock);
  }

  lowering_.functionFrame().switchTo(bodyBlock);
  pushLoop(endBlock, stepBlock, lowering_.scopes_.size());
  lowering_.stmtLowerer().emitStmt(stmt->body);
  lowering_.targets_.pop_back();

  lowering_.functionFrame().emitBlock(stepBlock);
  if (auto *decl = llvm::dyn_cast_or_null<VarDeclStmt>(stmt->init)) {
    for (VarDeclarator &d : decl->decls) {
      if (d.alias || !d.sym->boxed()) {
        continue;
      }
      const Type *type = ClosureLowerer::typeOf(d.sym);
      llvm::Value *slot = lowering_.boxSlots_.lookup(d.sym);
      llvm::Value *old = lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), slot);
      llvm::Value *box = lowering_.envLowerer().newBox(type);
      llvm::Value *value = lowering_.builder_.CreateLoad(
          lowering_.moduleBuilder().lower(type),
          lowering_.builder_.CreateStructGEP(lowering_.envLowerer().boxType(type), old, 2));
      lowering_.refCounts().retainValue(value, type);
      lowering_.builder_.CreateStore(
          value, lowering_.builder_.CreateStructGEP(lowering_.envLowerer().boxType(type), box, 2));
      if (lowering_.isCyclicBox(type)) {
        lowering_.storeLowerer().groupStore(box, value, type);
      }
      lowering_.builder_.CreateStore(box, slot);
      lowering_.builder_.CreateCall(lowering_.objReleaseFn_, {old});
    }
  }
  if (stmt->step) {
    lowering_.stmtLowerer().emitDiscarded(stmt->step);
  }
  lowering_.builder_.CreateBr(condBlock);

  lowering_.functionFrame().switchTo(endBlock);
  lowering_.functionFrame().popScope();
}

void LoopLowerer::emitForOf(ForOfStmt *stmt) {
  if (stmt->isAwait) {
    lowering_.asyncGeneratorLowerer().emitForAwait(stmt);
    return;
  }
  if (stmt->iterable->type->isIterator() || stmt->iterable->type->isTypedArray()) {
    lowering_.iteratorLowerer().emitIteratorForOf(stmt);
    return;
  }
  lowering_.functionFrame().pushScope();
  const Type *iterableType = stmt->iterable->type;
  llvm::Value *iterableSlot =
      lowering_.functionFrame().createManagedSlot(iterableType, "for.iterable");
  lowering_.builder_.CreateStore(lowering_.exprLowerer().emitExpr(stmt->iterable), iterableSlot);

  llvm::BasicBlock *condBlock = lowering_.functionFrame().newBlock("for.of.cond");
  llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("for.of.body");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("for.of.end");
  lowering_.arrayLowerer().emitArrayForOf(stmt, iterableSlot, condBlock, bodyBlock, endBlock);
  lowering_.functionFrame().switchTo(endBlock);
  lowering_.functionFrame().popScope();
}

}
