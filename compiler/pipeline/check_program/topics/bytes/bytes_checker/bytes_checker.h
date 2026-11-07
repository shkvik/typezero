#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

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
  static std::string utf8(const std::u16string &text);
  static std::optional<std::u16string> literalText(const Expr *e);
  static std::string rangeText(int64_t lo, int64_t hi);
  static ValueRange exact(int64_t v);
  static bool known(const ValueRange &r);
  static int64_t allOnes(int64_t v);
  static int64_t floorShift(int64_t v, int64_t n);

  CheckState &state;
};

}
