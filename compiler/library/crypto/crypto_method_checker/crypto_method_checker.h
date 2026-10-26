#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class CryptoMethodChecker {
public:
  explicit CryptoMethodChecker(Checker &checker);

  Expr *checkCryptoMethod(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

}
