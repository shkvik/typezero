#include "compiler/semantic/library/result/result_feature/result_feature.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/library/result/result_method_checker/result_method_checker.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

Expr *ResultFeature::checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                                     const Type *) const {
  return ResultChecker(state).checkResultStatic(call, member);
}

llvm::ArrayRef<llvm::StringLiteral> ResultFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"Result"};

  return globals;
}

Expr *ResultFeature::checkMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                                     const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isResult()) {
    return nullptr;
  }

  return ResultMethodChecker(state).checkResultMethod(call, member, expected);
}

Expr *ResultFeature::checkFunctionCall(SemanticState &state, CallExpr *call, NameExpr *name,
                                       const Type *expected) const {
  return ResultChecker(state).checkResultCall(call, name, expected);
}

}
