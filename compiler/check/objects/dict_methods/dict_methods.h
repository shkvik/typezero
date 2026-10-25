#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The methods of Map and Set, Object.fromEntries and groupBy.
class DictMethods {
public:
  explicit DictMethods(Checker &checker);

  // The methods of Map and Set, whose receiver is checked.
  Expr *checkDictMethod(CallExpr *call, MemberExpr *member);
  Expr *checkFromEntries(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkGroupBy(CallExpr *call, MemberExpr *member, bool map);
  Expr *checkMapStatic(CallExpr *call, MemberExpr *member);
  // Object.keys, Object.values and Object.entries of a dictionary, whose
  // argument is checked.
  Expr *checkDictObjectCall(CallExpr *call, llvm::StringRef fn);

private:
  Checker &checker_;
};

} // namespace tz
