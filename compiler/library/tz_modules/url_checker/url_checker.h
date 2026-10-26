#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class UrlChecker {
public:
  explicit UrlChecker(Checker &checker);

  void checkUrlNew(NewExpr *expr);
  void checkHrefSet(const Expr *value);
  static bool validUrlText(llvm::StringRef text, bool relative);
  static std::optional<std::string> literalText(const Expr *expr);
  static const Expr *unconverted(const Expr *expr);
  bool knownAbsolute(const Expr *expr) const;

private:
  Checker &checker_;
};

}
