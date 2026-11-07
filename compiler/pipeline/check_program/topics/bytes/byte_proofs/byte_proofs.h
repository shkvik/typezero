#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class ByteProofs {
public:
  explicit ByteProofs(CheckState &owner);

  ValueRange valueRange(const Expr *expr) const;
  bool provesFits(const Expr *expr, int64_t lo, int64_t hi) const;
  void checkFits(const Expr *value, const Type *target, llvm::StringRef what);
  bool provesWithin(const Expr *object, const Expr *offset, int64_t size) const;
  void checkView(const Expr *buffer, const Expr *offset, const Expr *length, int64_t size,
                 llvm::StringRef what);
  void addByteCallbackFacts(const FunctionDecl *fn);
  void checkByteCallbackResult(const FunctionDecl *fn, const Expr *value);

private:
  static std::string rangeText(int64_t lo, int64_t hi);
  static int64_t wrapped(Type::Bytes kind, int64_t v);
  static ValueRange exact(int64_t v);
  static bool known(const ValueRange &r);
  static ValueRange unite(const ValueRange &a, const ValueRange &b);
  static int64_t allOnes(int64_t v);
  static int64_t floorShift(int64_t v, int64_t n);

  ValueRange computedRange(const Expr *expr) const;

  CheckState &state;
};

}
