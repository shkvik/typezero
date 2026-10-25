#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// setEncoding, which turns a stream of bytes into one of strings, and pipe.
class SetEncodingChecker {
public:
  explicit SetEncodingChecker(Checker &checker);

  // setEncoding() of the program on a stream known by a constant (or a
  // read-only field of one): from that call on, the constant is seen as a
  // stream of strings (a StreamView); an error where the code before let the
  // stream go elsewhere, which sees its chunks as Buffers.
  Expr *checkSetEncoding(CallExpr *call, MemberExpr *member, ClassDecl *cls);
  // The class of node:stream that sees a stream of `cls` with an encoding.
  const Type *encodedStreamType(ClassDecl *cls, llvm::SMLoc loc);
  // The type a stream of `path` is seen as at `loc`, after setEncoding();
  // null if none.
  const Type *streamViewOf(const Checker::FieldPath &path, llvm::SMLoc loc) const;
  // `src.pipe(dest, options?)`: the chunks of src must be what dest takes.
  Expr *checkStreamPipe(CallExpr *call, MemberExpr *member, ClassDecl *cls);

private:
  Checker &checker_;
};

} // namespace tz
