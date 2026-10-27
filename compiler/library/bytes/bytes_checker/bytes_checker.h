#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class BytesChecker {
public:
  explicit BytesChecker(Checker &checker);

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
  Checker &checker_;
};

}
