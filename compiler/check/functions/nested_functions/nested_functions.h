#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class NestedFunctions {
public:
  explicit NestedFunctions(Checker &owner);

  NestedGroup *declareNested(BlockStmt *block);
  void finishGroup(NestedGroup *group);
  void noteUse(VarSymbol *var, llvm::SMLoc loc);
  FunctionDecl *namedFunction(const Expr *expr) const;

private:
  static void collectAssigned(const Stmt *stmt, std::vector<llvm::StringRef> &names);

  Checker &checker;
};

}
