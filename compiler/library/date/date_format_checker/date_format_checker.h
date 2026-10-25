#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Intl.DateTimeFormat and the locales and options of dates.
class DateFormatChecker {
public:
  explicit DateFormatChecker(Sema &sema);

  // The locale and the options of toLocaleString and the like and of
  // Intl.DateTimeFormat (`kind` is TZ_DATE_LOCALE_*); false after an error.
  bool checkLocaleArgs(std::vector<Expr *> &args, int kind);
  // Intl.DateTimeFormat: made with or without `new`, its methods, and the
  // types of what formatToParts and resolvedOptions give.
  Expr *checkDateFormatNew(llvm::SMLoc loc, std::vector<Expr *> &args);
  Expr *checkIntlCall(CallExpr *call, MemberExpr *member);
  Expr *checkDateFormatMethod(CallExpr *call, MemberExpr *member);
  static const Type *dateFormatPartType();
  static const Type *resolvedDateOptionsType();
  // Intl.DateTimeFormatOptions: every option, `T | undefined`.
  static const Type *dateOptionsType();

private:
  Sema &sema_;
};

} // namespace tz
