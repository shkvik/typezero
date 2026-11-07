#include "compiler/library/result/result_feature/result_feature.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/result/result_checker/result_checker.h"
#include "compiler/pipeline/check_program/topics/result/result_method_checker/result_method_checker.h"

namespace tz {

Expr *ResultFeature::checkStaticCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                     const Type *) const {
  return ResultChecker(state).checkResultStatic(call, member);
}

llvm::ArrayRef<llvm::StringLiteral> ResultFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"Result"};

  return globals;
}

Expr *ResultFeature::checkMethodCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                     const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isResult()) {
    return nullptr;
  }

  return ResultMethodChecker(state).checkResultMethod(call, member, expected);
}

Expr *ResultFeature::checkFunctionCall(CheckState &state, CallExpr *call, NameExpr *name,
                                       const Type *expected) const {
  return ResultChecker(state).checkResultCall(call, name, expected);
}

bool ResultFeature::lowers(Builtin builtin) const { return isResultBuiltin(builtin); }

llvm::Value *ResultFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.resultLowerer().emitResultCall(call);
}

}
