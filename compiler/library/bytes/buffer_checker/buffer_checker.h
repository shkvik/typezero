#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The methods of Buffer, TextEncoder and TextDecoder.
class BufferChecker {
public:
  explicit BufferChecker(Sema &sema);

  // The methods node gives Buffer; null for those of Uint8Array.
  Expr *checkBufferMethod(CallExpr *call, MemberExpr *member);
  Expr *checkTextMethod(CallExpr *call, MemberExpr *member);

private:
  Sema &sema_;
};

} // namespace tz
