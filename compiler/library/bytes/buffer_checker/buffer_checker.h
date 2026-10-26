#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class BufferChecker {
public:
  explicit BufferChecker(Checker &checker);

  Expr *checkBufferMethod(CallExpr *call, MemberExpr *member);
  Expr *checkTextMethod(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

}
