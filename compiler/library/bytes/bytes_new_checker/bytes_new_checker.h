#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class BytesNewChecker {
public:
  explicit BytesNewChecker(Checker &owner);

  Expr *checkBytesNew(NewExpr *expr, const Type *type);
  Expr *checkTextDecoderParse(CallExpr *call, MemberExpr *member);
  Expr *checkBytesValues(CallExpr *call, ArrayLitExpr *lit, const Type *type);
  bool checkBytesSource(Expr *source, const Type *target);

private:
  static std::string utf8(const std::u16string &text);
  static std::optional<std::u16string> literalText(const Expr *e);
  static std::string rangeText(int64_t lo, int64_t hi);
  static bool mayBeFatal(const Expr *options);

  Checker &checker;
  bool parsing = false;
};

}
