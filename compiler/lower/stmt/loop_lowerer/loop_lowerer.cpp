#include "compiler/lower/stmt/loop_lowerer/loop_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/errors/try_lowerer/try_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

LoopLowerer::LoopLowerer(CodeGen &codegen) : codegen_(codegen) {}

void LoopLowerer::pushLoop(llvm::BasicBlock *breakTarget, llvm::BasicBlock *continueTarget,
                       size_t scopeDepth) {
  codegen_.targets_.push_back({std::move(codegen_.pendingLabels_), /*isLoop=*/true, /*isSwitch=*/false, breakTarget,
                      continueTarget, scopeDepth});
  codegen_.pendingLabels_.clear();
}

void LoopLowerer::emitJump(Stmt *stmt) {
  bool isBreak = isa<BreakStmt>(stmt);
  llvm::StringRef label = isBreak ? cast<BreakStmt>(stmt)->label : cast<ContinueStmt>(stmt)->label;
  for (size_t i = codegen_.targets_.size(); i > 0; --i) {
    const CodeGen::JumpTarget &t = codegen_.targets_[i - 1];
    bool matches = !label.empty() ? llvm::is_contained(t.labels, label)
                   : isBreak      ? t.isLoop || t.isSwitch
                                  : t.isLoop;
    if (matches) {
      codegen_.tryLowerer().emitJumpTo(i - 1, isBreak);
      return;
    }
  }
  llvm_unreachable("Sema checks every break and continue");
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
    codegen_.pendingLabels_ = labels; // the loop or switch takes them
    codegen_.stmtLowerer().emitStmt(body);
    return;
  }
  // `label: { ... break label; ... }`
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("label.end");
  codegen_.targets_.push_back(
      {labels, /*isLoop=*/false, /*isSwitch=*/false, endBlock, nullptr, codegen_.scopes_.size()});
  codegen_.stmtLowerer().emitStmt(body);
  codegen_.targets_.pop_back();
  codegen_.functionFrame().emitBlock(endBlock);
}

void LoopLowerer::emitWhile(WhileStmt *stmt) {
  llvm::BasicBlock *condBlock = codegen_.functionFrame().newBlock("while.cond");
  llvm::BasicBlock *bodyBlock = codegen_.functionFrame().newBlock("while.body");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("while.end");

  codegen_.functionFrame().emitBlock(condBlock);
  llvm::Value *cond = codegen_.exprLowerer().emitExpr(stmt->cond);
  codegen_.builder_.CreateCondBr(cond, bodyBlock, endBlock);

  codegen_.functionFrame().switchTo(bodyBlock);
  pushLoop(endBlock, condBlock, codegen_.scopes_.size());
  codegen_.stmtLowerer().emitStmt(stmt->body);
  codegen_.targets_.pop_back();
  codegen_.functionFrame().branchTo(condBlock);

  codegen_.functionFrame().switchTo(endBlock);
}

void LoopLowerer::emitDoWhile(DoWhileStmt *stmt) {
  llvm::BasicBlock *bodyBlock = codegen_.functionFrame().newBlock("do.body");
  llvm::BasicBlock *condBlock = codegen_.functionFrame().newBlock("do.cond");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("do.end");

  codegen_.functionFrame().emitBlock(bodyBlock);
  pushLoop(endBlock, condBlock, codegen_.scopes_.size());
  codegen_.stmtLowerer().emitStmt(stmt->body);
  codegen_.targets_.pop_back();

  codegen_.functionFrame().emitBlock(condBlock);
  llvm::Value *cond = codegen_.exprLowerer().emitExpr(stmt->cond);
  codegen_.builder_.CreateCondBr(cond, bodyBlock, endBlock);

  codegen_.functionFrame().switchTo(endBlock);
}

void LoopLowerer::emitFor(ForStmt *stmt) {
  codegen_.functionFrame().pushScope(); // variables declared in the loop header
  if (stmt->init) {
    codegen_.stmtLowerer().emitStmt(stmt->init);
  }

  llvm::BasicBlock *condBlock = codegen_.functionFrame().newBlock("for.cond");
  llvm::BasicBlock *bodyBlock = codegen_.functionFrame().newBlock("for.body");
  llvm::BasicBlock *stepBlock = codegen_.functionFrame().newBlock("for.step");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("for.end");

  codegen_.functionFrame().emitBlock(condBlock);
  if (stmt->cond) {
    llvm::Value *cond = codegen_.exprLowerer().emitExpr(stmt->cond);
    codegen_.builder_.CreateCondBr(cond, bodyBlock, endBlock);
  } else {
    codegen_.builder_.CreateBr(bodyBlock);
  }

  codegen_.functionFrame().switchTo(bodyBlock);
  pushLoop(endBlock, stepBlock, codegen_.scopes_.size());
  codegen_.stmtLowerer().emitStmt(stmt->body);
  codegen_.targets_.pop_back();

  codegen_.functionFrame().emitBlock(stepBlock);
  // `for (let i ...)`: every iteration has an i of its own. A closure that
  // captured i keeps that iteration's box; the next iteration goes on in a
  // copy.
  if (auto *decl = llvm::dyn_cast_or_null<VarDeclStmt>(stmt->init)) {
    for (VarDeclarator &d : decl->decls) {
      if (d.alias || !d.sym->boxed()) {
        continue;
      }
      const Type *type = ClosureLowerer::typeOf(d.sym);
      llvm::Value *slot = codegen_.boxSlots_.lookup(d.sym);
      llvm::Value *old = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), slot);
      llvm::Value *box = codegen_.envLowerer().newBox(type);
      llvm::Value *value =
          codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(type), codegen_.builder_.CreateStructGEP(codegen_.envLowerer().boxType(type), old, 2));
      codegen_.refCounts().retainValue(value, type);
      codegen_.builder_.CreateStore(value, codegen_.builder_.CreateStructGEP(codegen_.envLowerer().boxType(type), box, 2));
      if (codegen_.isCyclicBox(type)) {
        codegen_.storeLowerer().groupStore(box, value, type); // inside the group of the box now
      }
      codegen_.builder_.CreateStore(box, slot);
      codegen_.builder_.CreateCall(codegen_.objReleaseFn_, {old});
    }
  }
  if (stmt->step) {
    codegen_.stmtLowerer().emitDiscarded(stmt->step);
  }
  codegen_.builder_.CreateBr(condBlock);

  codegen_.functionFrame().switchTo(endBlock);
  codegen_.functionFrame().popScope();
}

// `for (const x of iterable)`: the iterable is evaluated once and held in a
// hidden slot; each step reads the current length, as JavaScript's iterators
// do, so elements pushed by the body are visited too.
void LoopLowerer::emitForOf(ForOfStmt *stmt) {
  if (stmt->isAwait) {
    codegen_.asyncGeneratorLowerer().emitForAwait(stmt);
    return;
  }
  if (stmt->iterable->type->isIterator() || stmt->iterable->type->isTypedArray()) {
    codegen_.iteratorLowerer().emitIteratorForOf(stmt);
    return;
  }
  codegen_.functionFrame().pushScope(); // the hidden iterable
  const Type *iterableType = stmt->iterable->type;
  llvm::Value *iterableSlot = codegen_.functionFrame().createManagedSlot(iterableType, "for.iterable");
  codegen_.builder_.CreateStore(codegen_.exprLowerer().emitExpr(stmt->iterable), iterableSlot);

  llvm::BasicBlock *condBlock = codegen_.functionFrame().newBlock("for.of.cond");
  llvm::BasicBlock *bodyBlock = codegen_.functionFrame().newBlock("for.of.body");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("for.of.end");
  codegen_.arrayLowerer().emitArrayForOf(stmt, iterableSlot, condBlock, bodyBlock, endBlock);
  codegen_.functionFrame().switchTo(endBlock);
  codegen_.functionFrame().popScope();
}

} // namespace tz
