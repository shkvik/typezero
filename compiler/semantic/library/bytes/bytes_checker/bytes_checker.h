#pragma once

#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
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

  const Type *bytesTypeNamed(llvm::StringRef name) const;
  Expr *checkBufferEncoding(Expr *arg);
  Expr *checkBytesIndex(IndexExpr *index);
  void walkBytesStore(IndexExpr *index, const Expr *value);
  Expr *checkBytesMember(MemberExpr *member);
  Expr *checkBytesUnionMember(MemberExpr *member);
  Expr *checkBytesMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkBytesStaticMember(MemberExpr *member);
  Expr *bytesToString(Expr *expr);

private:
  using ResultKnownEncoding = Result<int, BytesCheckerKnownEncodingError>;
  using ResultBytesIndexing = Result<const Type *, BytesCheckerBytesIndexingError>;

  ResultBytesIndexing checkBytesIndexing(const Type *type) const;
  ResultKnownEncoding checkKnownEncoding(llvm::StringRef name) const;
  std::optional<std::u16string> literalText(const Expr *expr) const;

  Expr *reportNoMember(MemberExpr *member);
  Expr *keepObject(MemberExpr *member, Expr *value);
  IntLitExpr *makeIntLiteral(llvm::SMLoc loc, int64_t value);
  BoolLitExpr *makeBoolLiteral(llvm::SMLoc loc, bool value);
  Expr *checkViewMember(MemberExpr *member, std::optional<BuiltinProperty> property);
  Expr *checkDataViewMember(MemberExpr *member, std::optional<BuiltinProperty> property);
  Expr *checkArrayBufferMember(MemberExpr *member, std::optional<BuiltinProperty> property);
  Expr *checkTextEncoderMember(MemberExpr *member, std::optional<BuiltinProperty> property);
  Expr *checkTextDecoderMember(MemberExpr *member, std::optional<BuiltinProperty> property);
  bool hasUnionProperty(std::optional<BuiltinProperty> property, const Type *memberType) const;
  Expr *readBuiltin(MemberExpr *member, Builtin builtin, const Type *result,
                    std::vector<Expr *> args = {});

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
