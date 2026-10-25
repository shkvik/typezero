#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Tail calls.
class TailCallLowerer {
public:
  explicit TailCallLowerer(Lowering &lowering);

  void findTailGroups(llvm::ArrayRef<FunctionDecl *> functions);
  // The same for a closure: among its group, or itself.
  void findTailGroups(FunctionDecl *closure);
  void emitTailGroup(Lowering::TailGroup &group);
  // A call `value` returns as it is to a function of the group being
  // emitted; null if none.
  CallExpr *tailCall(Expr *value) const;
  bool hasTailCall(Expr *value) const;
  void emitTailReturn(Expr *value);
  void emitTailJump(CallExpr *call);

private:
  Lowering &lowering_;
  std::deque<Lowering::TailGroup> tailGroups_;
  llvm::DenseSet<const void *> tailSearched_;
};

} // namespace tz
