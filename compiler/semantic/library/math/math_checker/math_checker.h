#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/math/math_checker/math_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class MathChecker {
public:
  explicit MathChecker(SemanticState &owner);

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

  SemanticState &state;
};

}
