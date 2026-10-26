#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class IntlChecker {
public:
  explicit IntlChecker(Checker &checker);

  Expr *checkIntlNew(llvm::SMLoc loc, llvm::StringRef name, std::vector<Expr *> &args,
                     bool withNew);
  Expr *checkIntlFunction(CallExpr *call, MemberExpr *member);
  Expr *checkIntlStatic(CallExpr *call, MemberExpr *member);
  Expr *checkIntlMember(MemberExpr *member, const Type *expected);
  Expr *checkLocaleList(Expr *arg, llvm::StringRef what);
  Expr *segmentsArray(Expr *segments);
  static const Type *segmentDataType();
  static const Type *resolvedIntlOptionsType(IntlKind kind);
  static const Type *intlTypeNamed(llvm::StringRef name);

private:
  Checker &checker_;
};

}
