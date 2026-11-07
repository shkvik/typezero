#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/bytes/byte_proofs/byte_proofs_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ByteProofs {
public:
  using ResultValueFitsType = Result<const Expr *, ByteProofsValueFitsError>;

  explicit ByteProofs(CheckState &owner);

  IntegerRange valueRange(const Expr *expr) const;
  bool provesFits(const Expr *expr, int64_t lo, int64_t hi) const;
  ResultValueFitsType valueFits(const Expr *value, const Type *target, llvm::StringRef what);
  bool provesWithin(const Expr *object, const Expr *offset, int64_t size) const;
  void checkView(const Expr *buffer, const Expr *offset, const Expr *length, int64_t size,
                 llvm::StringRef what);
  void addByteCallbackFacts(const FunctionDecl *fn);
  void checkByteCallbackResult(const FunctionDecl *fn, const Expr *value);

private:
  static const VarSymbol *viewOwner(const Expr *expr, llvm::StringRef field,
                                    const Type **type = nullptr);
  static const Expr *withoutCasts(const Expr *expr);
  static bool sameView(const Expr *buffer, const Expr *offset, const Expr *length, int64_t size);
  using ResultOffsetAligned = Result<int64_t, ByteProofsOffsetAlignedError>;
  using ResultBufferAligned = Result<int64_t, ByteProofsBufferAlignedError>;

  static ResultOffsetAligned offsetAligned(std::optional<int64_t> start, int64_t size,
                                           llvm::StringRef what);
  static ResultBufferAligned bufferAligned(std::optional<int64_t> whole, int64_t size,
                                           llvm::StringRef what);
  static int64_t wrapped(Type::Bytes kind, int64_t v);
  static IntegerRange exact(int64_t v);
  static bool known(const IntegerRange &r);
  static IntegerRange unite(const IntegerRange &a, const IntegerRange &b);
  static int64_t allOnes(int64_t v);
  static int64_t floorShift(int64_t v, int64_t n);

  IntegerRange computedRange(const Expr *expr) const;

  CheckState &state;
};

}
