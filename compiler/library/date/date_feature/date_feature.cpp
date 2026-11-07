#include "compiler/library/date/date_feature/date_feature.h"
#include "compiler/library/date/date_lowerer/date_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/date/date_checker/date_checker.h"
#include "compiler/pipeline/check_program/topics/date/date_format_checker/date_format_checker.h"

namespace tz {

llvm::ArrayRef<llvm::StringLiteral> DateFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"Date"};

  return globals;
}

Expr *DateFeature::checkMethodCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                   const Type *) const {
  const Type *receiver = member->object->type;
  if (receiver->isDate()) {
    return DateChecker(state).checkDateMethod(call, member);
  }
  if (receiver->isDateFormat()) {
    return DateFormatChecker(state).checkDateFormatMethod(call, member);
  }

  return nullptr;
}

bool DateFeature::lowers(Builtin builtin) const { return isDateBuiltin(builtin); }

llvm::Value *DateFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.dateLowerer().emitDateCall(call);
}

}
