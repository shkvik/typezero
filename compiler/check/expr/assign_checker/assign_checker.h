#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class AssignChecker {
public:
  explicit AssignChecker(Checker &owner);

  Expr *checkAssign(AssignExpr *assign);
  Expr *checkLogicalAssign(AssignExpr *assign);
  Expr *checkUpdate(UpdateExpr *update);
  Expr *checkFieldLogicalAssign(AssignExpr *assign);

private:
  static std::pair<llvm::StringRef, llvm::StringRef> operandTexts(llvm::StringRef source,
                                                                  llvm::SMLoc op, size_t length);

  Checker &checker;
  bool logicalAssign = false;
};

}
