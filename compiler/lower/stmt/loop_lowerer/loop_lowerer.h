#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Loops, break, continue and labels.
class LoopLowerer {
public:
  explicit LoopLowerer(CodeGen &codegen);

  void emitWhile(WhileStmt *stmt);
  void emitDoWhile(DoWhileStmt *stmt);
  void emitFor(ForStmt *stmt);
  void emitForOf(ForOfStmt *stmt);
  void emitLabeled(LabeledStmt *stmt);
  void emitJump(Stmt *stmt);
  void pushLoop(llvm::BasicBlock *breakTarget, llvm::BasicBlock *continueTarget, size_t scopeDepth);

private:
  CodeGen &codegen_;
};

} // namespace tz
