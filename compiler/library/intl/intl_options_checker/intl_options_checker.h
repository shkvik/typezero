#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The locales and options given to Intl.
class IntlOptionsChecker {
public:
  explicit IntlOptionsChecker(Sema &sema);

  // The locale and the options at args[from] and after (see sema_intl.cpp);
  // `kind` None for those of supportedLocalesOf. False after an error.
  bool checkIntlArgs(std::vector<Expr *> &args, size_t from, IntlKind kind, llvm::StringRef what);

private:
  Sema &sema_;
};

} // namespace tz
