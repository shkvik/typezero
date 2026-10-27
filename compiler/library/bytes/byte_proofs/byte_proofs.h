#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ByteProofs {
public:
  explicit ByteProofs(Checker &checker);

  ValueRange valueRange(const Expr *expr) const;
  bool provesFits(const Expr *expr, int64_t lo, int64_t hi) const;
  void checkFits(const Expr *value, const Type *target, llvm::StringRef what);
  bool provesWithin(const Expr *object, const Expr *offset, int64_t size) const;
  void checkView(const Expr *buffer, const Expr *offset, const Expr *length, int64_t size,
                 llvm::StringRef what);
  void addByteCallbackFacts(const FunctionDecl *fn);
  void checkByteCallbackResult(const FunctionDecl *fn, const Expr *value);

private:
  Checker &checker_;
};

}
