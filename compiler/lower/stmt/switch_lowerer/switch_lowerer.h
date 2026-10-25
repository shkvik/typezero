#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// switch.
class SwitchLowerer {
public:
  explicit SwitchLowerer(CodeGen &codegen);

  void emitSwitch(SwitchStmt *stmt);

private:
  CodeGen &codegen_;
};

} // namespace tz
