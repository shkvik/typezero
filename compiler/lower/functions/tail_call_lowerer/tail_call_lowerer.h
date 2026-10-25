#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Tail calls.
class TailCallLowerer {
public:
  explicit TailCallLowerer(CodeGen &codegen);

  void findTailGroups(llvm::ArrayRef<FunctionDecl *> functions);
  // The same for a closure: among its group, or itself.
  void findTailGroups(FunctionDecl *closure);
  void emitTailGroup(CodeGen::TailGroup &group);
  // A call `value` returns as it is to a function of the group being
  // emitted; null if none.
  CallExpr *tailCall(Expr *value) const;
  bool hasTailCall(Expr *value) const;
  void emitTailReturn(Expr *value);
  void emitTailJump(CallExpr *call);

private:
  CodeGen &codegen_;
  std::deque<CodeGen::TailGroup> tailGroups_;
  llvm::DenseSet<const void *> tailSearched_;
};

} // namespace tz
