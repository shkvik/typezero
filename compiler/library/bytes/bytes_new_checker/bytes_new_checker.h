#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// new of typed arrays, DataView and Buffer.
class BytesNewChecker {
public:
  explicit BytesNewChecker(Sema &sema);

  Expr *checkBytesNew(NewExpr *expr, const Type *type);
  // The values of `new Uint8Array([1, 2])` and Uint8Array.of(1, 2), each
  // checked to fit; with spreads, an array of them first.
  Expr *checkBytesValues(CallExpr *call, ArrayLitExpr *lit, const Type *type);
  // A checked array, iterator or typed array whose elements a typed array of
  // `target` copies: reports them when they may not fit.
  bool checkBytesSource(Expr *source, const Type *target);

private:
  Sema &sema_;
};

} // namespace tz
