#include "compiler/library/math/math_feature/math_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/math/math_checker/math_checker.h"
#include "compiler/library/math/math_lowerer/math_lowerer.h"

namespace tz {

Expr *MathFeature::checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  if (!sema.callChecker().isGlobalName(member->object, "Math")) {
    return nullptr;
  }

  return sema.mathChecker().checkMathCall(call, member, expected);
}

llvm::ArrayRef<llvm::StringLiteral> MathFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"Math"};

  return globals;
}

bool MathFeature::lowers(Builtin builtin) const {
  return isMathBuiltin(builtin);
}

llvm::Value *MathFeature::lowerCall(CodeGen &codegen, CallExpr *call) const {
  return codegen.mathLowerer().emitMathCall(call);
}

} // namespace tz
