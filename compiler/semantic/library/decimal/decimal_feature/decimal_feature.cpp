#include "compiler/semantic/library/decimal/decimal_feature/decimal_feature.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

Expr *DecimalFeature::checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                                      const Type *) const {
  if (!CallChecker(state).isGlobalName(member->object, "decimal")) {
    return nullptr;
  }

  return DecimalChecker(state).checkDecimalStatic(call, member);
}

llvm::ArrayRef<llvm::StringLiteral> DecimalFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"decimal"};

  return globals;
}

Expr *DecimalFeature::checkValueMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                                           const Type *) const {
  const Type *receiver = member->object->type;
  if (!receiver->isDecimal()) {
    return nullptr;
  }

  return DecimalChecker(state).checkDecimalMethod(call, member);
}

}
