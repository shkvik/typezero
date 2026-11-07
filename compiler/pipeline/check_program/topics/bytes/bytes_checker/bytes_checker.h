#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/bytes/bytes_checker/bytes_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class BytesChecker {
public:
  explicit BytesChecker(CheckState &owner);

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

  CheckState &state;
};

}
