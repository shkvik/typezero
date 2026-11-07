#include "compiler/lower/stmt/switch_lowerer/switch_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/expr/equality_lowerer/equality_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"


namespace tz {

SwitchLowerer::SwitchLowerer(Lowering &owner)
    : lowering(owner) {}

void SwitchLowerer::emitSwitch(SwitchStmt *stmt) {
  const Type *type = stmt->discriminant->type;
  llvm::SmallVector<llvm::StringRef, 1> labels = std::move(this->lowering.pendingLabels_);
  this->lowering.pendingLabels_.clear();
  size_t depth = this->lowering.scopes_.size();
  this->lowering.functionFrame().pushScope();
  llvm::Value *value = this->lowering.exprLowerer().emitExpr(stmt->discriminant);
  llvm::Value *slot = nullptr;
  if (type->isManaged()) {
    slot = this->lowering.functionFrame().createManagedSlot(type, "switch.value");
    this->lowering.builder_.CreateStore(value, slot);
  }

  llvm::BasicBlock *exitBlock = this->lowering.functionFrame().newBlock("switch.exit");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("switch.end");
  std::vector<llvm::BasicBlock *> bodies;
  llvm::BasicBlock *defaultBody = exitBlock;
  for (const SwitchCase &c : stmt->cases) {
    bodies.push_back(
        this->lowering.functionFrame().newBlock(c.test ? "switch.case" : "switch.default"));
    if (!c.test) {
      defaultBody = bodies.back();
    }
  }

  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    const SwitchCase &c = stmt->cases[i];
    if (!c.test) {
      continue;
    }
    llvm::Value *test = this->lowering.exprLowerer().emitExpr(c.test);
    llvm::Value *current =
        slot ? this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(type), slot)
             : value;
    llvm::Value *equal;
    if (!c.compareType) {
      equal = type->isOptional()    ? this->lowering.builder_.CreateNot(
                                          this->lowering.optionalLowerer().optionalHas(current, type))
              : type->isUndefined() ? this->lowering.builder_.getTrue()
                                    : this->lowering.builder_.getFalse();
    } else {
      equal = this->lowering.equalityLowerer().emitValuesEqual(
          this->lowering.exprLowerer().emitConversion(current, type, c.compareType), test,
          c.compareType);
    }
    this->lowering.refCounts().releaseValue(test, c.test->type);
    llvm::BasicBlock *next = this->lowering.functionFrame().newBlock("switch.test");
    this->lowering.builder_.CreateCondBr(equal, bodies[i], next);
    this->lowering.functionFrame().switchTo(next);
  }
  this->lowering.builder_.CreateBr(defaultBody);

  this->lowering.targets_.push_back({labels, false, true, endBlock, nullptr, depth});
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    this->lowering.functionFrame().switchTo(bodies[i]);
    for (Stmt *s : stmt->cases[i].body) {
      this->lowering.stmtLowerer().emitStmt(s);
    }
    this->lowering.functionFrame().branchTo(i + 1 < bodies.size() ? bodies[i + 1] : exitBlock);
  }
  this->lowering.targets_.pop_back();
  this->lowering.functionFrame().switchTo(exitBlock);
  this->lowering.functionFrame().popScope();
  this->lowering.functionFrame().emitBlock(endBlock);
}

}
