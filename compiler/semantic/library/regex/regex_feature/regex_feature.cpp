#include "compiler/semantic/library/regex/regex_feature/regex_feature.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/library/regex/match_checker/match_checker.h"
#include "compiler/semantic/library/regex/regex_checker/regex_checker.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

Expr *RegexFeature::checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
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

Expr *RegexFeature::checkMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                                    const Type *) const {
  const Type *receiver = member->object->type;
  if (!receiver->isRegExp()) {
    return nullptr;
  }

  return RegexChecker(state).checkRegexMethod(call, member);
}

Expr *RegexFeature::methodReceiver(SemanticState &state, Expr *object) const {
  if (!object->type->isMatch()) {
    return nullptr;
  }

  return MatchChecker(state).matchItems(object);
}

}
