#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class BufferStaticChecker {
public:
  explicit BufferStaticChecker(Checker &checker);

  Expr *checkBytesFromWith(CallExpr *call, const Type *type, const std::string &what);
  Expr *checkBufferStatic(CallExpr *call, MemberExpr *member);
  Expr *checkBytesStatic(CallExpr *call, MemberExpr *member);
  Expr *checkEmbedBytes(CallExpr *call, NameExpr *name);

private:
  Checker &checker_;
};

}
