#include "compiler/lower/library/console/console_library_lowerer/console_library_lowerer.h"
#include "compiler/lower/library/console/console_lowerer/console_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool ConsoleLibraryLowerer::lowers(Builtin builtin) const {
  return builtin == Builtin::ConsoleLog || builtin == Builtin::ConsoleError;
}

llvm::Value *ConsoleLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  lowering.consoleLowerer().emitConsoleLog(call);

  return nullptr;
}

}
