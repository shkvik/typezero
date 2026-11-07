#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class BufferStaticChecker {
public:
  explicit BufferStaticChecker(CheckState &owner);

  Expr *checkBytesFromWith(CallExpr *call, const Type *type, const std::string &what);
  Expr *checkBufferStatic(CallExpr *call, MemberExpr *member);
  Expr *checkBytesStatic(CallExpr *call, MemberExpr *member);
  Expr *checkEmbedBytes(CallExpr *call, NameExpr *name);

private:
  static std::string utf8(const std::u16string &text);
  static std::optional<std::u16string> literalText(const Expr *e);
  static std::string rangeText(int64_t lo, int64_t hi);

  CheckState &state;
};

}
