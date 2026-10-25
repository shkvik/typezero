#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// new of the classes of node:stream.
class StreamNewChecker {
public:
  explicit StreamNewChecker(Checker &checker);

  // `new Readable({...})` and the others without type arguments: the chunk
  // types its options say (an encoding gives strings); null when the place
  // it goes gives them, or for another class.
  Expr *checkStreamNew(NewExpr *expr, ClassDecl *generic, const Type *expected);

private:
  Checker &checker_;
};

} // namespace tz
