#include "compiler/library/regex/regex_feature/regex_feature.h"
#include "compiler/library/regex/regex_lowerer/regex_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/regex/match_checker/match_checker.h"
#include "compiler/pipeline/check_program/topics/regex/regex_checker/regex_checker.h"

namespace tz {

Expr *RegexFeature::checkStaticCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                    const Type *) const {
  if (!CallChecker(state).isGlobalName(member->object, "RegExp")) {
    return nullptr;
  }

  return RegexChecker(state).checkRegExpStatic(call, member);
}

llvm::ArrayRef<llvm::StringLiteral> RegexFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"RegExp"};

  return globals;
}

Expr *RegexFeature::checkMethodCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                    const Type *) const {
  const Type *receiver = member->object->type;
  if (!receiver->isRegExp()) {
    return nullptr;
  }

  return RegexChecker(state).checkRegexMethod(call, member);
}

Expr *RegexFeature::methodReceiver(CheckState &state, Expr *object) const {
  if (!object->type->isMatch()) {
    return nullptr;
  }

  return MatchChecker(state).matchItems(object);
}

bool RegexFeature::lowers(Builtin builtin) const { return isRegexBuiltin(builtin); }

llvm::Value *RegexFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.regexLowerer().emitRegexCall(call);
}

}
