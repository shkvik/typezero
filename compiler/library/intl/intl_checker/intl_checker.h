#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/library/intl/intl_check_helpers/intl_check_helpers.h"

namespace tz {

class IntlChecker {
public:
  explicit IntlChecker(Checker &owner);

  Expr *checkIntlNew(llvm::SMLoc loc, llvm::StringRef name, std::vector<Expr *> &args,
                     bool withNew);
  Expr *checkIntlFunction(CallExpr *call, MemberExpr *member);
  Expr *checkIntlStatic(CallExpr *call, MemberExpr *member);
  Expr *checkIntlMember(MemberExpr *member, const Type *expected);
  Expr *checkLocaleList(Expr *arg, llvm::StringRef what);
  Expr *checkLocaleNew(NewExpr *expr, const Type *expected);
  Expr *checkLocaleStatic(CallExpr *call, MemberExpr *member, const Type *expected);
  bool checkLocaleArg(Expr *&arg, llvm::StringRef what);
  Expr *segmentsArray(Expr *segments);
  static const Type *segmentDataType();
  static const Type *resolvedIntlOptionsType(IntlKind kind);
  static const Type *intlTypeNamed(llvm::StringRef name);

private:
  static std::string utf8(const std::u16string &text);
  static std::optional<std::u16string> literalText(const Expr *e);
  static const IntlService *serviceNamed(llvm::StringRef name);
  static std::u16string utf16(llvm::StringRef text);
  static const Type *relativePartType();
  static const Expr *unconverted(const Expr *expr);
  static bool isStringLike(const Type *type);
  static const Type *numberOptionType(llvm::StringRef name);

  bool checkTags(const Expr *arg, llvm::StringRef what, bool constructor);

  Checker &checker;
};

}
