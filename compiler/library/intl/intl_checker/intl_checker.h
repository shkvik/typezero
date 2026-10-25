#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Intl beyond dates (docs/intl.md): types and new.
class IntlChecker {
public:
  explicit IntlChecker(Checker &checker);

  // The rest of Intl (sema_intl.cpp, docs/intl.md): Intl.NumberFormat,
  // Collator, PluralRules, RelativeTimeFormat, ListFormat, DisplayNames and
  // Segmenter, made with `new` (NumberFormat and Collator also without), and
  // their methods; the values `nf.format`, `collator.compare` and
  // `dtf.format`, bound to their objects; toLocaleString of numbers and
  // decimals; localeCompare and normalize of strings; getCanonicalLocales and
  // supportedLocalesOf.
  Expr *checkIntlNew(llvm::SMLoc loc, llvm::StringRef name, std::vector<Expr *> &args,
                     bool withNew);
  // `Intl.X(...)` other than DateTimeFormat, and Intl.getCanonicalLocales.
  Expr *checkIntlFunction(CallExpr *call, MemberExpr *member);
  // `Intl.X.supportedLocalesOf(...)`: the callee's object is `Intl.X`.
  Expr *checkIntlStatic(CallExpr *call, MemberExpr *member);
  Expr *checkIntlMember(MemberExpr *member, const Type *expected);
  Expr *checkLocaleList(Expr *arg, llvm::StringRef what);
  // The segments of Intl.Segmenter's segment(), where they are iterated: an
  // array of their data.
  Expr *segmentsArray(Expr *segments);
  static const Type *segmentDataType();
  static const Type *resolvedIntlOptionsType(IntlKind kind);
  // The type `Intl.<name>` names in an annotation (`Intl.NumberFormat`,
  // `Intl.NumberFormatOptions`...), or null.
  static const Type *intlTypeNamed(llvm::StringRef name);

private:
  Checker &checker_;
};

} // namespace tz
