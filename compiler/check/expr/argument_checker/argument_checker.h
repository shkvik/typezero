#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ArgumentChecker {
public:
  explicit ArgumentChecker(Checker &checker);

  Expr *checkArguments(CallExpr *call, llvm::ArrayRef<const Type *> params, size_t minArgs,
                       bool hasRest, llvm::SMLoc loc, const std::vector<bool> *checked = nullptr);
  Expr *checkArg(Expr *arg, const Type *to);
  Expr *checkAnyNumber(Expr *arg);
  bool checkArity(CallExpr *call, llvm::SMLoc loc, unsigned min, unsigned max);

private:
  Checker &checker_;
};

}
