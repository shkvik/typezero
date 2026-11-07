#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/functions/nested_functions/nested_functions_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class NestedFunctions {
public:
  explicit NestedFunctions(Checker &owner);

  NestedGroup *declareNested(BlockStmt *block);
  void finishGroup(NestedGroup *group);
  void noteUse(VarSymbol *var, llvm::SMLoc loc);
  FunctionDecl *namedFunction(const Expr *expr) const;

private:
  using ResultUseInOrder = Result<const NestedGroup::Use *, NestedFunctionsUseOrderError>;

  static ResultUseInOrder useOrder(const NestedGroup *group, const NestedGroup::Use &use);
  static VarSymbol *capturedLater(const NestedGroup *group, const NestedGroup::Use &use);
  static void collectAssigned(const Stmt *stmt, std::vector<llvm::StringRef> &names);

  Checker &checker;
};

}
