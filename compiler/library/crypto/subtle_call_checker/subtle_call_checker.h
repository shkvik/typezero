#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class SubtleCallChecker {
public:
  explicit SubtleCallChecker(Checker &owner);

  Expr *checkSubtleCall(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  static std::string utf8(const std::u16string &text);
  static std::optional<double> numberLiteral(const Expr *e);
  static const ObjectLitExpr::Property *property(const ObjectLitExpr *lit, llvm::StringRef name);

  Checker &checker;
};

}
