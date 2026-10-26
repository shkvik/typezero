#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class TailCallLowerer {
public:
  explicit TailCallLowerer(Lowering &lowering);

  void findTailGroups(llvm::ArrayRef<FunctionDecl *> functions);
  void findTailGroups(FunctionDecl *closure);
  void emitTailGroup(Lowering::TailGroup &group);
  CallExpr *tailCall(Expr *value) const;
  bool hasTailCall(Expr *value) const;
  void emitTailReturn(Expr *value);
  void emitTailJump(CallExpr *call);

private:
  Lowering &lowering_;
  std::deque<Lowering::TailGroup> tailGroups_;
  llvm::DenseSet<const void *> tailSearched_;
};

}
