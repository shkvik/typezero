#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/math/math_checker/math_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class MathChecker {
public:
  explicit MathChecker(CheckState &owner);

  Expr *checkMathCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkMinMaxSpread(CallExpr *call, const Type *hint);
  Expr *checkBitCall(CallExpr *call, MemberExpr *member);
  Expr *bitsArg(Expr *arg, unsigned bits, const Type *literal, const std::string &what);
  const Type *sharedType(std::vector<Expr *> &args, const std::vector<bool> &spreads,
                         const Type *common = nullptr);
  Expr *numericArg(Expr *arg, const Type *hint = nullptr);

private:
  using ResultWrapBitsWidth = Result<unsigned, MathCheckerWrapBitsWidthError>;
  using ResultRoundedDouble = Result<const Type *, MathCheckerRoundedDoubleError>;

  static ResultWrapBitsWidth wrapBitsWidth(llvm::StringRef name);
  static ResultRoundedDouble roundedDouble(llvm::StringRef name, const Type *type);

  CheckState &state;
};

}
