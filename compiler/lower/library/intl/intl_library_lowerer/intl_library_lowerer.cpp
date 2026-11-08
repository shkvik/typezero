#include "compiler/lower/library/intl/intl_library_lowerer/intl_library_lowerer.h"
#include "compiler/lower/library/intl/intl_call_lowerer/intl_call_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool IntlLibraryLowerer::lowers(Builtin builtin) const {
  return BuiltinGroups::isIntlBuiltin(builtin);
}

llvm::Value *IntlLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.intlCallLowerer().emitIntlCall(call);
}

}
