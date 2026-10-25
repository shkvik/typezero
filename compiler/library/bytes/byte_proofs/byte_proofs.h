#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The proofs that a value written into a typed array fits it (docs/bytes.md).
class ByteProofs {
public:
  explicit ByteProofs(Checker &checker);

  // What values an integer expression, checked, may have: from its form,
  // the elements it reads and the facts about the variables in it.
  ValueRange valueRange(const Expr *expr) const;
  bool provesFits(const Expr *expr, int64_t lo, int64_t hi) const;
  // Reports `value` (checked) stored in an element of the typed array type
  // `target` when it may not fit; `what` says where it goes.
  void checkFits(const Expr *value, const Type *target, llvm::StringRef what);
  // Whether 0 <= offset and offset + size <= the length of `object` (of a
  // DataView, an ArrayBuffer or a typed array).
  bool provesWithin(const Expr *object, const Expr *offset, int64_t size) const;
  // In the body of such a callback: the facts about its element, and the
  // check of what it returns.
  void addByteCallbackFacts(const FunctionDecl *fn);
  void checkByteCallbackResult(const FunctionDecl *fn, const Expr *value);

private:
  Checker &checker_;
};

} // namespace tz
