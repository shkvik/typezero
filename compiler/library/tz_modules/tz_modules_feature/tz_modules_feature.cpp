#include "compiler/library/tz_modules/tz_modules_feature/tz_modules_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/tz_modules/library_call_lowerer/library_call_lowerer.h"

namespace tz {

void TzModulesFeature::prepare(Sema &sema, SourceFile &file) const {
  sema.libraryCalls().prepareLibraries(file);
}

Expr *TzModulesFeature::checkFunctionCall(Sema &sema, CallExpr *call, NameExpr *name, const Type *expected) const {
  return sema.libraryCalls().checkLibCall(call, name);
}

bool TzModulesFeature::lowers(Builtin builtin) const {
  return isLibBuiltin(builtin);
}

llvm::Value *TzModulesFeature::lowerCall(CodeGen &codegen, CallExpr *call) const {
  return codegen.libraryCallLowerer().emitLibBuiltin(call);
}

} // namespace tz
