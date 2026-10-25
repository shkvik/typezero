#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The methods of the objects of node:crypto: hashes, ciphers, keys, certificates.
class CryptoMethodChecker {
public:
  explicit CryptoMethodChecker(Checker &checker);

  Expr *checkCryptoMethod(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

} // namespace tz
