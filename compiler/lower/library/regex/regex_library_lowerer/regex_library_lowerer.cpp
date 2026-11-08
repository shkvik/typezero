#include "compiler/lower/library/regex/regex_library_lowerer/regex_library_lowerer.h"
#include "compiler/lower/library/regex/regex_lowerer/regex_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool RegexLibraryLowerer::lowers(Builtin builtin) const {
  return BuiltinGroups::isRegexBuiltin(builtin);
}

llvm::Value *RegexLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.regexLowerer().emitRegexCall(call);
}

}
