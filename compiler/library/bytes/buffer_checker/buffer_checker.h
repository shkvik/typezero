#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class BufferChecker {
public:
  explicit BufferChecker(Checker &owner);

  Expr *checkBufferMethod(CallExpr *call, MemberExpr *member);
  Expr *checkTextMethod(CallExpr *call, MemberExpr *member);

private:
  static std::optional<std::u16string> literalText(const Expr *e);
  static std::string rangeText(int64_t lo, int64_t hi);

  Checker &checker;

  void checkWriteLength(const Expr *object, const Expr *offset, const Expr *length);
};

}
