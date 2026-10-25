#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The locales and options given to Intl.
class IntlOptionsChecker {
public:
  explicit IntlOptionsChecker(Checker &checker);

  // The locale and the options at args[from] and after (see sema_intl.cpp);
  // `kind` None for those of supportedLocalesOf. False after an error.
  bool checkIntlArgs(std::vector<Expr *> &args, size_t from, IntlKind kind, llvm::StringRef what);

private:
  Checker &checker_;
};

} // namespace tz
