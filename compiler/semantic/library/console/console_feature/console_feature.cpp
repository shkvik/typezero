#include "compiler/semantic/library/console/console_feature/console_feature.h"
#include "compiler/semantic/library/console/console_checker/console_checker.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

Expr *ConsoleFeature::checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                                      const Type *) const {
  return ConsoleChecker(state).checkConsoleCall(call, member);
}

llvm::ArrayRef<llvm::StringLiteral> ConsoleFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"console"};

  return globals;
}

}
