#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class IntlOptionsChecker {
public:
  explicit IntlOptionsChecker(Checker &checker);

  bool checkIntlArgs(std::vector<Expr *> &args, size_t from, IntlKind kind, llvm::StringRef what);

private:
  Checker &checker_;
};

}
