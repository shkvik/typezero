#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The calls of Intl objects.
class IntlCallLowerer {
public:
  explicit IntlCallLowerer(CodeGen &codegen);

  // The rest of Intl (codegen_intl.cpp): objects of the runtime
  // (runtime/library/intl/intl/intl.h).
  llvm::Value *emitIntlCall(CallExpr *call);

private:
  CodeGen &codegen_;
};

} // namespace tz
