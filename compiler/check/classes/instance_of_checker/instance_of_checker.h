#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// instanceof.
class InstanceOfChecker {
public:
  explicit InstanceOfChecker(Sema &sema);

  Expr *checkInstanceOf(BinaryExpr *bin);
  // `x instanceof Uint8Array` (or another type of binary data, `bytes`).
  Expr *checkBytesInstanceOf(BinaryExpr *bin, const Type *bytes);

private:
  Sema &sema_;
};

} // namespace tz
