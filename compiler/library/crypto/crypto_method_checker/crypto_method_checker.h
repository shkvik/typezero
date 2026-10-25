#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The methods of the objects of node:crypto: hashes, ciphers, keys, certificates.
class CryptoMethodChecker {
public:
  explicit CryptoMethodChecker(Sema &sema);

  Expr *checkCryptoMethod(CallExpr *call, MemberExpr *member);

private:
  Sema &sema_;
};

} // namespace tz
