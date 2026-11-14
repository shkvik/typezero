#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/SmallPtrSet.h"

namespace tz {

class ConstructorContext {
public:
  void markRootSuper(const CallExpr *call);
  void markCallReported(const CallExpr *call);


  bool isRootSuper(const CallExpr *call) const;
  bool isCallReported(const CallExpr *call) const;

private:
  llvm::SmallPtrSet<const CallExpr *, 4> rootSupers;
  llvm::SmallPtrSet<const CallExpr *, 4> reportedCalls;
};

}
