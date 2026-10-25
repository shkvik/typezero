#include "compiler/library/date/date_feature/date_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/date/date_format_checker/date_format_checker.h"
#include "compiler/library/date/date_lowerer/date_lowerer.h"

namespace tz {

llvm::ArrayRef<llvm::StringLiteral> DateFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"Date"};

  return globals;
}

Expr *DateFeature::checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  const Type *receiver = member->object->type;
  if (receiver->isDate()) {
    return sema.dateChecker().checkDateMethod(call, member);
  }
  if (receiver->isDateFormat()) {
    return sema.dateFormatChecker().checkDateFormatMethod(call, member);
  }

  return nullptr;
}

bool DateFeature::lowers(Builtin builtin) const {
  return isDateBuiltin(builtin);
}

llvm::Value *DateFeature::lowerCall(CodeGen &codegen, CallExpr *call) const {
  return codegen.dateLowerer().emitDateCall(call);
}

} // namespace tz
