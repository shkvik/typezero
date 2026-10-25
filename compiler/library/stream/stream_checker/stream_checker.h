#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// node:stream (docs/streams.md): its intrinsics and calls.
class StreamChecker {
public:
  explicit StreamChecker(Sema &sema);

  // `__tz_chunk<T>(u)` and the other intrinsics of node:stream.
  Expr *checkStreamIntrinsic(CallExpr *call, NameExpr *name, const Type *typeArg);
  // `(error?: Error) => void`, the callback of write() and end().
  const Type *streamCallbackType();
  // A chunk the program pushes to (writes to, `writing`) a stream of
  // `chunk`, converted to an unknown; with `eof`, undefined ends it.
  // `checked`: the argument is checked already.
  Expr *streamChunkArg(Expr *arg, const Type *chunk, bool eof, bool writing, llvm::StringRef method,
                       bool checked);
  // push(), unshift(), write() and end() of the program on a stream: calls
  // of the code of Stream with the chunk as an unknown; null for the other
  // methods.
  Expr *checkStreamCall(CallExpr *call, MemberExpr *member, ClassDecl *cls);

private:
  Sema &sema_;
};

} // namespace tz
