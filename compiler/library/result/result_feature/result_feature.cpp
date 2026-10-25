#include "compiler/library/result/result_feature/result_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/library/result/result_async_checker/result_async_checker.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "compiler/library/result/result_method_checker/result_method_checker.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"

namespace tz {

Expr *ResultFeature::checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  if (Expr *checked = sema.resultAsyncChecker().checkResultAsyncStatic(call, member, expected)) {
    return checked;
  }

  return sema.resultChecker().checkResultStatic(call, member);
}

llvm::ArrayRef<llvm::StringLiteral> ResultFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"Result"};

  return globals;
}

Expr *ResultFeature::checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isResult()) {
    return nullptr;
  }

  return sema.resultMethodChecker().checkResultMethod(call, member, expected);
}

Expr *ResultFeature::checkFunctionCall(Sema &sema, CallExpr *call, NameExpr *name, const Type *expected) const {
  return sema.resultChecker().checkResultCall(call, name, expected);
}

bool ResultFeature::lowers(Builtin builtin) const {
  return isResultBuiltin(builtin);
}

llvm::Value *ResultFeature::lowerCall(CodeGen &codegen, CallExpr *call) const {
  return codegen.resultLowerer().emitResultCall(call);
}

} // namespace tz
