#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class FunctionChecker {
public:
  explicit FunctionChecker(Checker &owner);

  void declareFunction(FunctionDecl *fn, VarSymbol *var = nullptr, bool scoped = true);
  void checkFunctionBody(FunctionDecl *fn);

private:
  Checker &checker;
};

}
