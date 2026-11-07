#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class CryptoMethodChecker {
public:
  explicit CryptoMethodChecker(CheckState &owner);

  Expr *checkCryptoMethod(CallExpr *call, MemberExpr *member);

private:
  CheckState &state;
};

}
