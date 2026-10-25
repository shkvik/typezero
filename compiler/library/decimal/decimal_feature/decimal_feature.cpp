#include "compiler/library/decimal/decimal_feature/decimal_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

Expr *DecimalFeature::checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                      const Type * /*expected*/) const {
  if (!checker.callChecker().isGlobalName(member->object, "decimal")) {
    return nullptr;
  }

  return checker.decimalChecker().checkDecimalStatic(call, member);
}

llvm::ArrayRef<llvm::StringLiteral> DecimalFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"decimal"};

  return globals;
}

Expr *DecimalFeature::checkValueMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                           const Type * /*expected*/) const {
  const Type *receiver = member->object->type;
  if (!receiver->isDecimal()) {
    return nullptr;
  }

  return checker.decimalChecker().checkDecimalMethod(call, member);
}

bool DecimalFeature::lowers(Builtin builtin) const { return isDecimalBuiltin(builtin); }

llvm::Value *DecimalFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.decimalLowerer().emitDecimalCall(call);
}

} // namespace tz
