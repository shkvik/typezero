#include "compiler/library/result/result_feature/result_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/library/result/result_method_checker/result_method_checker.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

Expr *ResultFeature::checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                     const Type *) const {
  return checker.resultChecker().checkResultStatic(call, member);
}

llvm::ArrayRef<llvm::StringLiteral> ResultFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"Result"};

  return globals;
}

Expr *ResultFeature::checkMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                     const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isResult()) {
    return nullptr;
  }

  return checker.resultMethodChecker().checkResultMethod(call, member, expected);
}

Expr *ResultFeature::checkFunctionCall(Checker &checker, CallExpr *call, NameExpr *name,
                                       const Type *expected) const {
  return checker.resultChecker().checkResultCall(call, name, expected);
}

bool ResultFeature::lowers(Builtin builtin) const { return isResultBuiltin(builtin); }

llvm::Value *ResultFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.resultLowerer().emitResultCall(call);
}

}
