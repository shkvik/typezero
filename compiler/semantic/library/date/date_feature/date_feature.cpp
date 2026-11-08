#include "compiler/semantic/library/date/date_feature/date_feature.h"
#include "compiler/semantic/library/date/date_checker/date_checker.h"
#include "compiler/semantic/library/date/date_format_checker/date_format_checker.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

llvm::ArrayRef<llvm::StringLiteral> DateFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"Date"};

  return globals;
}

Expr *DateFeature::checkMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
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

}
