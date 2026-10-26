#include "compiler/library/date/date_feature/date_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/date/date_format_checker/date_format_checker.h"
#include "compiler/library/date/date_lowerer/date_lowerer.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

llvm::ArrayRef<llvm::StringLiteral> DateFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"Date"};

  return globals;
}

Expr *DateFeature::checkMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                   const Type *) const {
  const Type *receiver = member->object->type;
  if (receiver->isDate()) {
    return checker.dateChecker().checkDateMethod(call, member);
  }
  if (receiver->isDateFormat()) {
    return checker.dateFormatChecker().checkDateFormatMethod(call, member);
  }

  return nullptr;
}

bool DateFeature::lowers(Builtin builtin) const { return isDateBuiltin(builtin); }

llvm::Value *DateFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.dateLowerer().emitDateCall(call);
}

}
