#include "compiler/library/math/math_feature/math_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/math/math_checker/math_checker.h"
#include "compiler/library/math/math_lowerer/math_lowerer.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

Expr *MathFeature::checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                   const Type *expected) const {
  if (!checker.callChecker().isGlobalName(member->object, "Math")) {
    return nullptr;
  }

  return checker.mathChecker().checkMathCall(call, member, expected);
}

llvm::ArrayRef<llvm::StringLiteral> MathFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"Math"};

  return globals;
}

bool MathFeature::lowers(Builtin builtin) const { return isMathBuiltin(builtin); }

llvm::Value *MathFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.mathLowerer().emitMathCall(call);
}

}
