#include "compiler/library/regex/regex_feature/regex_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/regex/match_checker/match_checker.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "compiler/library/regex/regex_lowerer/regex_lowerer.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

Expr *RegexFeature::checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                    const Type *) const {
  if (!checker.callChecker().isGlobalName(member->object, "RegExp")) {
    return nullptr;
  }

  return checker.regexChecker().checkRegExpStatic(call, member);
}

llvm::ArrayRef<llvm::StringLiteral> RegexFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"RegExp"};

  return globals;
}

Expr *RegexFeature::checkMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                    const Type *) const {
  const Type *receiver = member->object->type;
  if (!receiver->isRegExp()) {
    return nullptr;
  }

  return checker.regexChecker().checkRegexMethod(call, member);
}

Expr *RegexFeature::methodReceiver(Checker &checker, Expr *object) const {
  if (!object->type->isMatch()) {
    return nullptr;
  }

  return checker.matchChecker().matchItems(object);
}

bool RegexFeature::lowers(Builtin builtin) const { return isRegexBuiltin(builtin); }

llvm::Value *RegexFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.regexLowerer().emitRegexCall(call);
}

}
