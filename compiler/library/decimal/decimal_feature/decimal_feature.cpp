#include "compiler/library/decimal/decimal_feature/decimal_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"

namespace tz {

Expr *DecimalFeature::checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  if (!sema.callChecker().isGlobalName(member->object, "decimal")) {
    return nullptr;
  }

  return sema.decimalChecker().checkDecimalStatic(call, member);
}

llvm::ArrayRef<llvm::StringLiteral> DecimalFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"decimal"};

  return globals;
}

Expr *DecimalFeature::checkValueMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isDecimal()) {
    return nullptr;
  }

  return sema.decimalChecker().checkDecimalMethod(call, member);
}

bool DecimalFeature::lowers(Builtin builtin) const {
  return isDecimalBuiltin(builtin);
}

llvm::Value *DecimalFeature::lowerCall(CodeGen &codegen, CallExpr *call) const {
  return codegen.decimalLowerer().emitDecimalCall(call);
}

} // namespace tz
