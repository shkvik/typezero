#include "compiler/library/intl/intl_feature/intl_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/date/date_format_checker/date_format_checker.h"
#include "compiler/library/intl/intl_checker/intl_checker.h"
#include "compiler/library/intl/intl_method_checker/intl_method_checker.h"
#include "compiler/library/intl/intl_call_lowerer/intl_call_lowerer.h"

namespace tz {

Expr *IntlFeature::checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  if (sema.callChecker().isGlobalName(member->object, "Intl")) {
    return sema.dateFormatChecker().checkIntlCall(call, member);
  }
  if (auto *inner = llvm::dyn_cast<MemberExpr>(member->object);
      inner && sema.callChecker().isGlobalName(inner->object, "Intl")) {
    return sema.intlChecker().checkIntlStatic(call, member);
  }

  return nullptr;
}

llvm::ArrayRef<llvm::StringLiteral> IntlFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"Intl"};

  return globals;
}

Expr *IntlFeature::checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isIntl()) {
    return nullptr;
  }

  return sema.intlMethodChecker().checkIntlMethod(call, member);
}

Expr *IntlFeature::checkValueMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  const Type *receiver = member->object->type;
  if (receiver->isString() && (member->name == "localeCompare" || member->name == "normalize")) {
    return sema.intlMethodChecker().checkStringLocale(call, member);
  }
  if ((receiver->isNumeric() || receiver->isDecimal()) && member->name == "toLocaleString") {
    return sema.intlMethodChecker().checkNumberLocale(call, member);
  }

  return nullptr;
}

bool IntlFeature::lowers(Builtin builtin) const {
  return isIntlBuiltin(builtin);
}

llvm::Value *IntlFeature::lowerCall(CodeGen &codegen, CallExpr *call) const {
  return codegen.intlCallLowerer().emitIntlCall(call);
}

} // namespace tz
