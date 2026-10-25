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
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

SwitchLowerer::SwitchLowerer(Lowering &lowering) : lowering_(lowering) {}

// The discriminant is evaluated once and compared with each test in order;
// the first one equal to it jumps to its case, and no match to `default`.
// The case bodies follow each other, so control falls through.
void SwitchLowerer::emitSwitch(SwitchStmt *stmt) {
  const Type *type = stmt->discriminant->type;
  llvm::SmallVector<llvm::StringRef, 1> labels = std::move(lowering_.pendingLabels_);
  lowering_.pendingLabels_.clear();
  size_t depth = lowering_.scopes_.size();
  lowering_.functionFrame().pushScope(); // the variables of the cases
  llvm::Value *value = lowering_.exprLowerer().emitExpr(stmt->discriminant);
  llvm::Value *slot = nullptr;
  if (type->isManaged()) {
    // Released whichever way control leaves the switch.
    slot = lowering_.functionFrame().createManagedSlot(type, "switch.value");
    lowering_.builder_.CreateStore(value, slot);
  }

  llvm::BasicBlock *exitBlock = lowering_.functionFrame().newBlock("switch.exit");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("switch.end");
  std::vector<llvm::BasicBlock *> bodies;
  llvm::BasicBlock *defaultBody = exitBlock;
  for (const SwitchCase &c : stmt->cases) {
    bodies.push_back(lowering_.functionFrame().newBlock(c.test ? "switch.case" : "switch.default"));
    if (!c.test) {
      defaultBody = bodies.back();
    }
  }

  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    const SwitchCase &c = stmt->cases[i];
    if (!c.test) {
      continue;
    }
    llvm::Value *test = lowering_.exprLowerer().emitExpr(c.test);
    llvm::Value *current =
        slot ? lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(type), slot) : value;
    llvm::Value *equal;
    if (!c.compareType) { // `case undefined:`
      equal =
          type->isOptional()
              ? lowering_.builder_.CreateNot(lowering_.optionalLowerer().optionalHas(current, type))
          : type->isUndefined() ? lowering_.builder_.getTrue()
                                : lowering_.builder_.getFalse();
    } else {
      equal = lowering_.equalityLowerer().emitValuesEqual(
          lowering_.exprLowerer().emitConversion(current, type, c.compareType), test,
          c.compareType);
    }
    lowering_.refCounts().releaseValue(test, c.test->type);
    llvm::BasicBlock *next = lowering_.functionFrame().newBlock("switch.test");
    lowering_.builder_.CreateCondBr(equal, bodies[i], next);
    lowering_.functionFrame().switchTo(next);
  }
  lowering_.builder_.CreateBr(defaultBody);

  lowering_.targets_.push_back(
      {labels, /*isLoop=*/false, /*isSwitch=*/true, endBlock, nullptr, depth});
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    lowering_.functionFrame().switchTo(bodies[i]);
    for (Stmt *s : stmt->cases[i].body) {
      lowering_.stmtLowerer().emitStmt(s);
    }
    lowering_.functionFrame().branchTo(i + 1 < bodies.size() ? bodies[i + 1] : exitBlock);
  }
  lowering_.targets_.pop_back();
  lowering_.functionFrame().switchTo(exitBlock);
  lowering_.functionFrame().popScope();
  lowering_.functionFrame().emitBlock(endBlock);
}

} // namespace tz
