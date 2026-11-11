#pragma once

#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/ArrayRef.h"

namespace tz {

class SemanticParts;
class IndexProofs;
class ConstantFolder;

class ByteProofs {
public:
  using ResultValueFitsType = Result<const Expr *, ByteProofsValueFitsError>;

  ByteProofs(SemanticState &owner, SemanticParts &all);

  IntegerRange valueRange(const Expr *expr) const;
  bool provesFits(const Expr *expr, int64_t lo, int64_t hi) const;
  ResultValueFitsType valueFits(const Expr *value, const Type *target, llvm::StringRef what);
  bool provesWithin(const Expr *object, const Expr *offset, int64_t size) const;
  void checkView(const Expr *buffer, const Expr *offset, const Expr *length, int64_t size,
                 llvm::StringRef what);
  void addByteCallbackFacts(const FunctionDecl *fn);
  void checkByteCallbackResult(const FunctionDecl *fn, const Expr *value);

private:
  static const VarSymbol *viewOwner(const Expr *expr, BuiltinProperty field,
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

  static NodeChildren<const Expr *> rangeParts(const Expr *e);
  static NodeChildren<const Expr *> callRangeParts(const CallExpr *call);
  static IntegerRange clampToType(const Expr *e, IntegerRange r);
  IntegerRange nodeRange(const Expr *e, std::vector<IntegerRange> &answers) const;
  IntegerRange computedRange(const Expr *e, llvm::ArrayRef<IntegerRange> parts) const;
  static IntegerRange castRange(const CastExpr *conv, const IntegerRange &operand);
  IntegerRange nameRange(const NameExpr *name) const;
  static IntegerRange memberRange(const MemberExpr *member);
  static IntegerRange elementRangeOf(const Type *type);
  static IntegerRange callRange(const CallExpr *call, llvm::ArrayRef<IntegerRange> parts);
  static IntegerRange minMaxRange(const CallExpr *call, llvm::ArrayRef<IntegerRange> parts);
  static IntegerRange lowerOf(const IntegerRange &a, const IntegerRange &b);
  static IntegerRange upperOf(const IntegerRange &a, const IntegerRange &b);
  static IntegerRange unaryRange(const UnaryExpr *unary, const IntegerRange &operand);
  static IntegerRange binaryRange(const BinaryExpr *bin, llvm::ArrayRef<IntegerRange> parts);

  SemanticState &state;
  IndexProofs *indexProofs = nullptr;
  ConstantFolder *constantFolder = nullptr;
};

}
