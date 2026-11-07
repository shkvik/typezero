#include "compiler/library/math/math_feature/math_feature.h"
#include "compiler/library/math/math_lowerer/math_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/math/math_checker/math_checker.h"

namespace tz {

Expr *MathFeature::checkStaticCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                   const Type *expected) const {
  auto *inner = llvm::dyn_cast<MemberExpr>(member->object);
  if (CallChecker(state).isGlobalName(member->object, "Bit") ||
      (inner && CallChecker(state).isGlobalName(inner->object, "Bit"))) {
    return MathChecker(state).checkBitCall(call, member);
  }
  if (!CallChecker(state).isGlobalName(member->object, "Math")) {
    return nullptr;
  }

  return MathChecker(state).checkMathCall(call, member, expected);
}

llvm::ArrayRef<llvm::StringLiteral> MathFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"Math", "Bit"};

  return globals;
}

bool MathFeature::lowers(Builtin builtin) const { return isMathBuiltin(builtin); }

llvm::Value *MathFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.mathLowerer().emitMathCall(call);
}

}
