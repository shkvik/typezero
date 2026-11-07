#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/result/result_checker/result_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ResultChecker {
public:
  using ResultValueGetsUsed = Result<const Expr *, ResultCheckerUsedValueError>;

  explicit ResultChecker(CheckState &owner);

  Expr *checkResultCall(CallExpr *call, NameExpr *name, const Type *expected);
  Expr *checkResultStatic(CallExpr *call, MemberExpr *member);
  Expr *checkResultMember(MemberExpr *member);
  Expr *checkOkErr(CallExpr *call, NameExpr *name, bool isOk, const Type *expected);
  ResultValueGetsUsed usedValue(const Expr *expr, bool callback = false, llvm::SMLoc at = {});
  void addResultFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;
  Expr *markCallFailed(CallExpr *call);
  void checkArgsLoosely(CallExpr *call);

private:
  using ResultStaticIsKnown = Result<llvm::StringRef, ResultCheckerStaticMethodError>;
  using ResultOkErrTypeArgs = Result<const NameExpr *, ResultCheckerOkErrTypeArgsError>;

  static ResultOkErrTypeArgs okErrTypeArgs(const NameExpr *name);
  static ResultStaticIsKnown staticMethod(const NameExpr *name, llvm::StringRef method);

  static const Type *halfParam(const Type *half);
  static bool givesType(const Type *half);
  static std::string receiverText(const Expr *expr);
  static const Expr *unconverted(const Expr *expr);

  CheckState &state;
};

}
