#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Buffer.from, alloc, concat and the statics of typed arrays; embedBytes.
class BufferStaticChecker {
public:
  explicit BufferStaticChecker(Sema &sema);

  // `Uint8Array.from(xs, f)`, whose source and callback are args.
  Expr *checkBytesFromWith(CallExpr *call, const Type *type, const std::string &what);
  Expr *checkBufferStatic(CallExpr *call, MemberExpr *member);
  // Uint8Array.from, Buffer.alloc...; null if `member` is no static member
  // of binary data.
  Expr *checkBytesStatic(CallExpr *call, MemberExpr *member);
  Expr *checkEmbedBytes(CallExpr *call, NameExpr *name);

private:
  Sema &sema_;
};

} // namespace tz
