#include "compiler/library/intl/intl_feature/intl_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/date/date_format_checker/date_format_checker.h"
#include "compiler/library/intl/intl_call_lowerer/intl_call_lowerer.h"
#include "compiler/library/intl/intl_checker/intl_checker.h"
#include "compiler/library/intl/intl_method_checker/intl_method_checker.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

Expr *IntlFeature::checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                   const Type * /*expected*/) const {
  if (checker.callChecker().isGlobalName(member->object, "Intl")) {
    return checker.dateFormatChecker().checkIntlCall(call, member);
  }
  if (auto *inner = llvm::dyn_cast<MemberExpr>(member->object);
      inner && checker.callChecker().isGlobalName(inner->object, "Intl")) {
    return checker.intlChecker().checkIntlStatic(call, member);
  }

  return nullptr;
}

llvm::ArrayRef<llvm::StringLiteral> IntlFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"Intl"};

  return globals;
}

Expr *IntlFeature::checkMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                   const Type * /*expected*/) const {
  const Type *receiver = member->object->type;
  if (!receiver->isIntl()) {
    return nullptr;
  }

  return checker.intlMethodChecker().checkIntlMethod(call, member);
}

Expr *IntlFeature::checkValueMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                        const Type * /*expected*/) const {
  const Type *receiver = member->object->type;
  if (receiver->isString() && (member->name == "localeCompare" || member->name == "normalize")) {
    return checker.intlMethodChecker().checkStringLocale(call, member);
  }
  if ((receiver->isNumeric() || receiver->isDecimal()) && member->name == "toLocaleString") {
    return checker.intlMethodChecker().checkNumberLocale(call, member);
  }

  return nullptr;
}

bool IntlFeature::lowers(Builtin builtin) const { return isIntlBuiltin(builtin); }

llvm::Value *IntlFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.intlCallLowerer().emitIntlCall(call);
}

} // namespace tz
