#include "compiler/semantic/library/math/math_feature/math_feature.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/library/math/math_checker/math_checker.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

Expr *MathFeature::checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
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

}
