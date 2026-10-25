#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// console.log and its kin.
class ConsoleLowerer {
public:
  explicit ConsoleLowerer(CodeGen &codegen);

  void emitConsoleLog(CallExpr *call);

private:
  CodeGen &codegen_;
};

} // namespace tz
