#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Functions declared inside functions: hoisting, and the groups that capture each other.
class NestedFunctions {
public:
  explicit NestedFunctions(Sema &sema);

  NestedGroup *declareNested(BlockStmt *block);
  void finishGroup(NestedGroup *group);
  // A use of a variable: the functions between its owner and here capture it.
  void noteUse(VarSymbol *var, llvm::SMLoc loc);
  // The function a name refers to directly, if any.
  FunctionDecl *namedFunction(const Expr *expr) const;

private:
  Sema &sema_;
};

} // namespace tz
