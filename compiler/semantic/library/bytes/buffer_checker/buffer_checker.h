#pragma once

#include "compiler/program/library/bytes/buffer_method_table/buffer_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/bytes/buffer_checker/buffer_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class SemanticParts;
class Prelude;
class ByteProofs;
class ExprChecker;
class IndexProofs;
class BytesChecker;
class MemberChecker;
class TypeRelations;
class ConstantFolder;
class ArgumentChecker;
struct BytesNumberMethod;

class BufferChecker {
public:
  BufferChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkBufferMethod(CallExpr *call, MemberExpr *member);
  Expr *checkTextMethod(CallExpr *call, MemberExpr *member);

private:
  struct BufferCall {
    CallExpr *call = nullptr;
    MemberExpr *member = nullptr;
    const Type *bufferType = nullptr;
    llvm::StringRef name;
  };

  struct EncodingAndPositions {
    Expr *encoding = nullptr;
    std::vector<Expr *> positions;
  };

  using ResultCountOfBytes = Result<int64_t, BufferCheckerCountOfBytesError>;
  using ResultDecodedInput = Result<const Type *, BufferCheckerDecodedInputError>;
  using ResultVarValueFits = Result<const Expr *, BufferCheckerVarValueFitsError>;

  ResultDecodedInput checkDecodedInput(const Type *type) const;
  ResultCountOfBytes checkCountOfBytes(std::optional<int64_t> count, llvm::StringRef name) const;
  std::optional<std::u16string> literalText(const Expr *expr) const;
  Expr *toBuiltinCall(CallExpr *call, Builtin builtin, const Type *result) const;

  Expr *checkBytesArg(Expr *arg);
  Expr *checkEncode(CallExpr *call, MemberExpr *member);
  Expr *checkDecode(CallExpr *call, MemberExpr *member);
  Expr *checkEncodeInto(CallExpr *call, MemberExpr *member);
  Expr *reportSlice(const BufferCall &buffer);
  Expr *checkCopy(const BufferCall &buffer);
  Expr *checkFill(const BufferCall &buffer);
  Expr *checkWrite(const BufferCall &buffer);
  Expr *checkToJson(const BufferCall &buffer);
  Expr *checkEquals(const BufferCall &buffer);
  Expr *checkCompare(const BufferCall &buffer);
  Expr *checkToString(const BufferCall &buffer);
  Expr *checkNumberMethod(const BufferCall &buffer);
  Expr *checkSwap(const BufferCall &buffer, int width);
  void walkPositionArgs(CallExpr *call, size_t from);
  IntLitExpr *makeIntLiteral(llvm::SMLoc loc, int64_t value);
  BoolLitExpr *makeBoolLiteral(llvm::SMLoc loc, bool value);
  Expr *checkValueArg(const BufferCall &buffer, Expr *arg);
  Expr *checkSearch(const BufferCall &buffer, BytesSearchMode mode);
  EncodingAndPositions splitWriteArgs(const BufferCall &buffer);
  EncodingAndPositions splitValueArgs(const BufferCall &buffer);
  void reportEncodingWithoutText(const Expr *value, const Expr *encoding);
  Expr *checkFixedNumber(const BufferCall &buffer, const BytesNumberMethod &number);
  Expr *checkVariableNumber(const BufferCall &buffer, const BytesNumberMethod &number);
  ResultVarValueFits checkVarValueFits(const Expr *value, int64_t width, bool isSigned);
  void reportUnprovenUpTo(const IndexProofs::ResultUpToWasProven &upTo, llvm::SMLoc at);
  void reportUnprovenBetween(const IndexProofs::ResultBetweenProven &between, llvm::SMLoc at);

  SemanticState &state;
  Prelude *prelude = nullptr;
  ByteProofs *byteProofs = nullptr;
  ExprChecker *exprChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  BytesChecker *bytesChecker = nullptr;
  MemberChecker *memberChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConstantFolder *constantFolder = nullptr;
  ArgumentChecker *argumentChecker = nullptr;

  void walkWriteLength(const Expr *object, const Expr *offset, const Expr *length);
};

}
