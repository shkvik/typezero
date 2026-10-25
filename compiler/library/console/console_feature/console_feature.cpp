#include "compiler/library/console/console_feature/console_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/console/console_checker/console_checker.h"
#include "compiler/library/console/console_lowerer/console_lowerer.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

Expr *ConsoleFeature::checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                      const Type * /*expected*/) const {
  return checker.consoleChecker().checkConsoleCall(call, member);
}

llvm::ArrayRef<llvm::StringLiteral> ConsoleFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"console"};

  return globals;
}

bool ConsoleFeature::lowers(Builtin builtin) const {
  return builtin == Builtin::ConsoleLog || builtin == Builtin::ConsoleError;
}

llvm::Value *ConsoleFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  lowering.consoleLowerer().emitConsoleLog(call);

  return nullptr;
}

} // namespace tz
