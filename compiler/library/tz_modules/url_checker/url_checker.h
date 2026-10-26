#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class UrlChecker {
public:
  explicit UrlChecker(Checker &checker);

  void checkUrlNew(NewExpr *expr);
  static bool validUrlText(llvm::StringRef text, bool relative);

private:
  bool knownAbsolute(const Expr *expr) const;

  Checker &checker_;
};

}
