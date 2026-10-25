#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Functions declared inside functions: hoisting, and the groups that capture each other.
class NestedFunctions {
public:
  explicit NestedFunctions(Checker &checker);

  NestedGroup *declareNested(BlockStmt *block);
  void finishGroup(NestedGroup *group);
  // A use of a variable: the functions between its owner and here capture it.
  void noteUse(VarSymbol *var, llvm::SMLoc loc);
  // The function a name refers to directly, if any.
  FunctionDecl *namedFunction(const Expr *expr) const;

private:
  Checker &checker_;
};

} // namespace tz
