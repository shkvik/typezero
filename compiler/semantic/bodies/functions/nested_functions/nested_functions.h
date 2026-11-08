#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/functions/nested_functions/nested_functions_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class NestedFunctions {
public:
  explicit NestedFunctions(SemanticState &owner);

  NestedGroup *declareNested(BlockStmt *block);
  void finishGroup(NestedGroup *group);
  void noteUse(VarSymbol *var, llvm::SMLoc loc);
  FunctionDecl *namedFunction(const Expr *expr) const;

private:
  using ResultUseInOrder = Result<const NestedGroup::Use *, NestedFunctionsUseOrderError>;

  static ResultUseInOrder useOrder(const NestedGroup *group, const NestedGroup::Use &use);
  static VarSymbol *capturedLater(const NestedGroup *group, const NestedGroup::Use &use);
  static void collectAssigned(const Stmt *stmt, std::vector<llvm::StringRef> &names);

  SemanticState &state;
};

}
