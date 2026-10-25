#include "compiler/library/process/process_feature/process_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"

namespace tz {

Expr *ProcessFeature::checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  return sema.processChecker().checkProcessCall(call, member);
}

llvm::ArrayRef<llvm::StringLiteral> ProcessFeature::typeArgFreeGlobals() const {
  static const llvm::StringLiteral globals[] = {"process"};

  return globals;
}

bool ProcessFeature::lowers(Builtin builtin) const {
  return isProcessBuiltin(builtin);
}

llvm::Value *ProcessFeature::lowerCall(CodeGen &codegen, CallExpr *call) const {
  return codegen.processLowerer().emitProcessBuiltin(call);
}

} // namespace tz
