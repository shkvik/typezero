#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// instanceof.
class InstanceOfChecker {
public:
  explicit InstanceOfChecker(Checker &checker);

  Expr *checkInstanceOf(BinaryExpr *bin);
  // `x instanceof Uint8Array` (or another type of binary data, `bytes`).
  Expr *checkBytesInstanceOf(BinaryExpr *bin, const Type *bytes);

private:
  Checker &checker_;
};

} // namespace tz
