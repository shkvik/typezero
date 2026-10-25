#include "compiler/lower/stmt/switch_lowerer/switch_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/equality_lowerer/equality_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
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

SwitchLowerer::SwitchLowerer(CodeGen &codegen) : codegen_(codegen) {}

// The discriminant is evaluated once and compared with each test in order;
// the first one equal to it jumps to its case, and no match to `default`.
// The case bodies follow each other, so control falls through.
void SwitchLowerer::emitSwitch(SwitchStmt *stmt) {
  const Type *type = stmt->discriminant->type;
  llvm::SmallVector<llvm::StringRef, 1> labels = std::move(codegen_.pendingLabels_);
  codegen_.pendingLabels_.clear();
  size_t depth = codegen_.scopes_.size();
  codegen_.functionFrame().pushScope(); // the variables of the cases
  llvm::Value *value = codegen_.exprLowerer().emitExpr(stmt->discriminant);
  llvm::Value *slot = nullptr;
  if (type->isManaged()) {
    // Released whichever way control leaves the switch.
    slot = codegen_.functionFrame().createManagedSlot(type, "switch.value");
    codegen_.builder_.CreateStore(value, slot);
  }

  llvm::BasicBlock *exitBlock = codegen_.functionFrame().newBlock("switch.exit");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("switch.end");
  std::vector<llvm::BasicBlock *> bodies;
  llvm::BasicBlock *defaultBody = exitBlock;
  for (const SwitchCase &c : stmt->cases) {
    bodies.push_back(codegen_.functionFrame().newBlock(c.test ? "switch.case" : "switch.default"));
    if (!c.test) {
      defaultBody = bodies.back();
    }
  }

  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    const SwitchCase &c = stmt->cases[i];
    if (!c.test) {
      continue;
    }
    llvm::Value *test = codegen_.exprLowerer().emitExpr(c.test);
    llvm::Value *current = slot ? codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(type), slot) : value;
    llvm::Value *equal;
    if (!c.compareType) { // `case undefined:`
      equal = type->isOptional()    ? codegen_.builder_.CreateNot(codegen_.optionalLowerer().optionalHas(current, type))
              : type->isUndefined() ? codegen_.builder_.getTrue()
                                    : codegen_.builder_.getFalse();
    } else {
      equal = codegen_.equalityLowerer().emitValuesEqual(codegen_.exprLowerer().emitConversion(current, type, c.compareType), test, c.compareType);
    }
    codegen_.refCounts().releaseValue(test, c.test->type);
    llvm::BasicBlock *next = codegen_.functionFrame().newBlock("switch.test");
    codegen_.builder_.CreateCondBr(equal, bodies[i], next);
    codegen_.functionFrame().switchTo(next);
  }
  codegen_.builder_.CreateBr(defaultBody);

  codegen_.targets_.push_back({labels, /*isLoop=*/false, /*isSwitch=*/true, endBlock, nullptr, depth});
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    codegen_.functionFrame().switchTo(bodies[i]);
    for (Stmt *s : stmt->cases[i].body) {
      codegen_.stmtLowerer().emitStmt(s);
    }
    codegen_.functionFrame().branchTo(i + 1 < bodies.size() ? bodies[i + 1] : exitBlock);
  }
  codegen_.targets_.pop_back();
  codegen_.functionFrame().switchTo(exitBlock);
  codegen_.functionFrame().popScope();
  codegen_.functionFrame().emitBlock(endBlock);
}

} // namespace tz
