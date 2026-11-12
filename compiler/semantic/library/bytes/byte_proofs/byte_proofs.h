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
  bool provesFits(const Expr *expr, int64_t low, int64_t high) const;
  ResultValueFitsType valueFits(const Expr *value, const Type *target, llvm::StringRef what);
  bool provesWithin(const Expr *object, const Expr *offset, int64_t size) const;
  void walkView(const Expr *buffer, const Expr *offset, const Expr *length, int64_t size,
                llvm::StringRef what);
  void addByteCallbackFacts(const FunctionDecl *functionDecl);
  using ResultByteCallbackChecked = Result<void, ByteProofsValueFitsError>;

  ResultByteCallbackChecked checkByteCallbackResult(const FunctionDecl *functionDecl,
                                                    const Expr *value);

private:
  const VarSymbol *viewOwner(const Expr *expr, BuiltinProperty field,
                             const Type **type = nullptr) const;
  const Expr *withoutCasts(const Expr *expr) const;
  bool sameView(const Expr *buffer, const Expr *offset, const Expr *length, int64_t size) const;
  using ResultOffsetAligned = Result<int64_t, ByteProofsOffsetAlignedError>;
  using ResultBufferAligned = Result<int64_t, ByteProofsBufferAlignedError>;

  ResultOffsetAligned offsetAligned(std::optional<int64_t> start, int64_t size,
                                    llvm::StringRef what) const;
  ResultBufferAligned bufferAligned(std::optional<int64_t> whole, int64_t size,
                                    llvm::StringRef what) const;
  int64_t wrapped(Type::Bytes kind, int64_t v) const;
  IntegerRange exact(int64_t v) const;
  bool known(const IntegerRange &r) const;
  IntegerRange unite(const IntegerRange &a, const IntegerRange &b) const;
  int64_t allOnes(int64_t v) const;
  int64_t floorShift(int64_t v, int64_t n) const;

  NodeChildren<const Expr *> rangeParts(const Expr *expr) const;
  NodeChildren<const Expr *> callRangeParts(const CallExpr *call) const;
  IntegerRange clampToType(const Expr *expr, IntegerRange r) const;
  IntegerRange nodeRange(const Expr *expr, std::vector<IntegerRange> &answers) const;
  IntegerRange computedRange(const Expr *expr, llvm::ArrayRef<IntegerRange> parts) const;
  IntegerRange castRange(const CastExpr *conversion, const IntegerRange &operand) const;
  IntegerRange nameRange(const NameExpr *name) const;
  IntegerRange memberRange(const MemberExpr *member) const;
  IntegerRange elementRangeOf(const Type *type) const;
  IntegerRange callRange(const CallExpr *call, llvm::ArrayRef<IntegerRange> parts) const;
  IntegerRange minMaxRange(const CallExpr *call, llvm::ArrayRef<IntegerRange> parts) const;
  IntegerRange lowerOf(const IntegerRange &a, const IntegerRange &b) const;
  IntegerRange upperOf(const IntegerRange &a, const IntegerRange &b) const;
  IntegerRange unaryRange(const UnaryExpr *unary, const IntegerRange &operand) const;
  IntegerRange binaryRange(const BinaryExpr *binary, llvm::ArrayRef<IntegerRange> parts) const;

  SemanticState &state;
  IndexProofs *indexProofs = nullptr;
  ConstantFolder *constantFolder = nullptr;
};

}
