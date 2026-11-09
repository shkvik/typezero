#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/bytes/buffer_checker/buffer_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

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

class BufferChecker {
public:
  BufferChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkBufferMethod(CallExpr *call, MemberExpr *member);
  Expr *checkTextMethod(CallExpr *call, MemberExpr *member);

private:
  using ResultCountOfBytes = Result<int64_t, BufferCheckerCountOfBytesError>;
  using ResultDecodedInput = Result<const Type *, BufferCheckerDecodedInputError>;

  static ResultDecodedInput decodedInput(const Type *type);
  static ResultCountOfBytes countOfBytes(std::optional<int64_t> count, llvm::StringRef name);
  static std::optional<std::u16string> literalText(const Expr *e);

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

  void checkWriteLength(const Expr *object, const Expr *offset, const Expr *length);
};

}
