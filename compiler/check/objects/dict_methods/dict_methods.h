#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class DictMethods {
public:
  explicit DictMethods(Checker &checker);

  Expr *checkDictMethod(CallExpr *call, MemberExpr *member);
  Expr *checkFromEntries(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkGroupBy(CallExpr *call, MemberExpr *member, bool map);
  Expr *checkMapStatic(CallExpr *call, MemberExpr *member);
  Expr *checkDictObjectCall(CallExpr *call, llvm::StringRef fn);

private:
  Checker &checker_;
};

}
