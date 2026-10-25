#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Typed arrays, DataView and Buffer: their types, indices and members.
class BytesChecker {
public:
  explicit BytesChecker(Checker &checker);

  // Binary data (sema_bytes.cpp, docs/bytes.md): typed arrays, Buffer,
  // ArrayBuffer, DataView, TextEncoder and TextDecoder, objects the runtime
  // makes. An element is read as an element of an array is: `u[i]` proven
  // in range is the element, otherwise it may be undefined. A write needs
  // that proof, and every value stored in an element (by index, fill, set,
  // a constructor, DataView.set…, Buffer.write…) must be proven to fit in
  // it: literals, masks, checks above, elements of a narrower kind.
  //
  // The type a name in an annotation or after `new` stands for, or null.
  static const Type *bytesTypeNamed(llvm::StringRef name);
  // An encoding of Buffer: a literal one becomes its number (an int
  // literal), another string is checked when the program runs.
  Expr *checkBufferEncoding(Expr *arg);
  Expr *checkBytesIndex(IndexExpr *index);
  // `u[i] = v`, `u[i] op= v` and `u[i]++` once the value is checked: the
  // index must be in range and the result must fit.
  void checkBytesStore(IndexExpr *index, const Expr *value, std::optional<BinaryOp> op,
                       llvm::SMLoc loc);
  Expr *checkBytesMember(MemberExpr *member);
  // `d.byteLength` of `Uint8Array | DataView | ArrayBuffer`, and the other
  // properties every member of a union of binary data has.
  Expr *checkBytesUnionMember(MemberExpr *member);
  Expr *checkBytesMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkBytesStaticMember(MemberExpr *member);
  // String(u) and `${u}`: the elements joined, or the text of a Buffer.
  Expr *bytesToString(Expr *expr);

private:
  Checker &checker_;
};

} // namespace tz
