#include "compiler/library/json/json_feature/json_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/json/json_static_checker/json_static_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"

namespace tz {

Expr *JsonFeature::checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  if (sema.callChecker().isGlobalName(member->object, "JSON")) {
    return sema.jsonStaticChecker().checkJsonStatic(call, member);
  }
  if (sema.callChecker().isGlobalName(member->object, "Array") && member->name == "isArray") {
    return sema.jsonChecker().checkArrayIsArray(call, member);
  }

  return nullptr;
}

Expr *JsonFeature::checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->nonOptional()->isJson()) {
    return nullptr;
  }

  return sema.jsonChecker().reportJsonMethod(call, member);
}

Expr *JsonFeature::checkFunctionCall(Sema &sema, CallExpr *call, NameExpr *name, const Type *expected) const {
  return sema.jsonStaticChecker().checkJsonGlobal(call, name);
}

bool JsonFeature::lowers(Builtin builtin) const {
  return isJsonBuiltin(builtin);
}

llvm::Value *JsonFeature::lowerCall(CodeGen &codegen, CallExpr *call) const {
  return codegen.jsonLowerer().emitJsonCall(call);
}

} // namespace tz
