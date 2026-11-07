#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class TailCallLowerer {
public:
  explicit TailCallLowerer(Lowering &owner);

  void findTailGroups(llvm::ArrayRef<FunctionDecl *> functions);
  void findTailGroups(FunctionDecl *closure);
  void emitTailGroup(Lowering::TailGroup &group);
  CallExpr *tailCall(Expr *value) const;
  bool hasTailCall(Expr *value) const;
  void emitTailReturn(Expr *value);
  void emitTailJump(CallExpr *call);

private:
  Lowering &lowering;
  std::deque<Lowering::TailGroup> tailGroups;
  llvm::DenseSet<const void *> tailSearched;
};

}
