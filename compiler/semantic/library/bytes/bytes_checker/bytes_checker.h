#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class ExprChecker;
class IndexProofs;
class LiteralTypes;
class BufferChecker;
class MemberChecker;
class ConstantFolder;
class ProcessChecker;
class ArgumentChecker;
class TypedArrayChecker;

class BytesChecker {
public:
  BytesChecker(SemanticState &owner, SemanticParts &all);

  static const Type *bytesTypeNamed(llvm::StringRef name);
  Expr *checkBufferEncoding(Expr *arg);
  Expr *checkBytesIndex(IndexExpr *index);
  void checkBytesStore(IndexExpr *index, const Expr *value);
  Expr *checkBytesMember(MemberExpr *member);
  Expr *checkBytesUnionMember(MemberExpr *member);
  Expr *checkBytesMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkBytesStaticMember(MemberExpr *member);
  Expr *bytesToString(Expr *expr);

private:
  using ResultKnownEncoding = Result<int, BytesCheckerKnownEncodingError>;
  using ResultBytesIndexing = Result<const Type *, BytesCheckerBytesIndexingError>;

  static ResultBytesIndexing bytesIndexing(const Type *type);
  static ResultKnownEncoding knownEncoding(llvm::StringRef name);
  static std::optional<std::u16string> literalText(const Expr *e);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  LiteralTypes *literalTypes = nullptr;
  BufferChecker *bufferChecker = nullptr;
  MemberChecker *memberChecker = nullptr;
  ConstantFolder *constantFolder = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  TypedArrayChecker *typedArrayChecker = nullptr;
};

}
