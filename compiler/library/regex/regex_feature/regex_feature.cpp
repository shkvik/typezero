#include "compiler/library/regex/regex_feature/regex_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "compiler/library/regex/match_checker/match_checker.h"
#include "compiler/library/regex/regex_lowerer/regex_lowerer.h"

namespace tz {

Expr *RegexFeature::checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  if (!sema.callChecker().isGlobalName(member->object, "RegExp")) {
    return nullptr;
  }

  return sema.regexChecker().checkRegExpStatic(call, member);
}

llvm::ArrayRef<llvm::StringLiteral> RegexFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"RegExp"};

  return globals;
}

Expr *RegexFeature::checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isRegExp()) {
    return nullptr;
  }

  return sema.regexChecker().checkRegexMethod(call, member);
}

Expr *RegexFeature::methodReceiver(Sema &sema, Expr *object) const {
  // The methods of arrays work on a match: its captures are an array.
  if (!object->type->isMatch()) {
    return nullptr;
  }

  return sema.matchChecker().matchItems(object);
}

bool RegexFeature::lowers(Builtin builtin) const {
  return isRegexBuiltin(builtin);
}

llvm::Value *RegexFeature::lowerCall(CodeGen &codegen, CallExpr *call) const {
  return codegen.regexLowerer().emitRegexCall(call);
}

} // namespace tz
