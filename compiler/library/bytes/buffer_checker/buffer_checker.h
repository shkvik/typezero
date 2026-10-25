#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The methods of Buffer, TextEncoder and TextDecoder.
class BufferChecker {
public:
  explicit BufferChecker(Checker &checker);

  // The methods node gives Buffer; null for those of Uint8Array.
  Expr *checkBufferMethod(CallExpr *call, MemberExpr *member);
  Expr *checkTextMethod(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

} // namespace tz
