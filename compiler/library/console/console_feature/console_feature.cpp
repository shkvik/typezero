#include "compiler/library/console/console_feature/console_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/library/console/console_checker/console_checker.h"
#include "compiler/library/console/console_lowerer/console_lowerer.h"

namespace tz {

Expr *ConsoleFeature::checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  return sema.consoleChecker().checkConsoleCall(call, member);
}

llvm::ArrayRef<llvm::StringLiteral> ConsoleFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"console"};

  return globals;
}

bool ConsoleFeature::lowers(Builtin builtin) const {
  return builtin == Builtin::ConsoleLog || builtin == Builtin::ConsoleError;
}

llvm::Value *ConsoleFeature::lowerCall(CodeGen &codegen, CallExpr *call) const {
  codegen.consoleLowerer().emitConsoleLog(call);

  return nullptr;
}

} // namespace tz
