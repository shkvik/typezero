#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class ResultChecker {
public:
  explicit ResultChecker(CheckState &owner);

  Expr *checkResultCall(CallExpr *call, NameExpr *name, const Type *expected);
  Expr *checkResultStatic(CallExpr *call, MemberExpr *member);
  Expr *checkResultMember(MemberExpr *member);
  Expr *checkOkErr(CallExpr *call, NameExpr *name, bool isOk, const Type *expected);
  void checkDiscarded(const Expr *expr, bool callback = false, llvm::SMLoc at = {});
  void addResultFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;
  Expr *reject(CallExpr *call, llvm::SMLoc loc, const std::string &why);
  void checkArgsLoosely(CallExpr *call);

private:
  static const Type *halfParam(const Type *half);
  static bool givesType(const Type *half);
  static std::string receiverText(const Expr *expr);
  static const Expr *unconverted(const Expr *expr);

  CheckState &state;
};

}
