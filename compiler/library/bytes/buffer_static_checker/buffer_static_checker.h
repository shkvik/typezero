#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Buffer.from, alloc, concat and the statics of typed arrays; embedBytes.
class BufferStaticChecker {
public:
  explicit BufferStaticChecker(Checker &checker);

  // `Uint8Array.from(xs, f)`, whose source and callback are args.
  Expr *checkBytesFromWith(CallExpr *call, const Type *type, const std::string &what);
  Expr *checkBufferStatic(CallExpr *call, MemberExpr *member);
  // Uint8Array.from, Buffer.alloc...; null if `member` is no static member
  // of binary data.
  Expr *checkBytesStatic(CallExpr *call, MemberExpr *member);
  Expr *checkEmbedBytes(CallExpr *call, NameExpr *name);

private:
  Checker &checker_;
};

} // namespace tz
