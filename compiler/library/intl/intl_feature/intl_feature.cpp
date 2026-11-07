#include "compiler/library/intl/intl_feature/intl_feature.h"
#include "compiler/library/intl/intl_call_lowerer/intl_call_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/date/date_format_checker/date_format_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/intl/intl_checker/intl_checker.h"
#include "compiler/pipeline/check_program/topics/intl/intl_method_checker/intl_method_checker.h"

namespace tz {

Expr *IntlFeature::checkStaticCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                   const Type *expected) const {
  if (CallChecker(state).isGlobalName(member->object, "Intl")) {
    return DateFormatChecker(state).checkIntlCall(call, member);
  }
  if (auto *inner = llvm::dyn_cast<MemberExpr>(member->object);
      inner && CallChecker(state).isGlobalName(inner->object, "Intl")) {
    return inner->name == "Locale" ? IntlChecker(state).checkLocaleStatic(call, member, expected)
                                   : IntlChecker(state).checkIntlStatic(call, member);
  }

  return nullptr;
}

llvm::ArrayRef<llvm::StringLiteral> IntlFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"Intl"};

  return globals;
}

Expr *IntlFeature::checkMethodCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                   const Type *) const {
  const Type *receiver = member->object->type;
  if (!receiver->isIntl()) {
    return nullptr;
  }

  return IntlMethodChecker(state).checkIntlMethod(call, member);
}

Expr *IntlFeature::checkValueMethodCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                        const Type *) const {
  const Type *receiver = member->object->type;
  if (receiver->isString() && (member->name == "localeCompare" || member->name == "normalize")) {
    return IntlMethodChecker(state).checkStringLocale(call, member);
  }
  if ((receiver->isNumeric() || receiver->isDecimal()) && member->name == "toLocaleString") {
    return IntlMethodChecker(state).checkNumberLocale(call, member);
  }

  return nullptr;
}

bool IntlFeature::lowers(Builtin builtin) const { return isIntlBuiltin(builtin); }

llvm::Value *IntlFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.intlCallLowerer().emitIntlCall(call);
}

}
