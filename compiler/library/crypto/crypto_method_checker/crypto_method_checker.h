#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class CryptoMethodChecker {
public:
  explicit CryptoMethodChecker(Checker &owner);

  Expr *checkCryptoMethod(CallExpr *call, MemberExpr *member);

private:
  Checker &checker;
};

}
