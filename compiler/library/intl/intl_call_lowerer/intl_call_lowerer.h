#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The calls of Intl objects.
class IntlCallLowerer {
public:
  explicit IntlCallLowerer(Lowering &lowering);

  // The rest of Intl (codegen_intl.cpp): objects of the runtime
  // (runtime/library/intl/intl/intl.h).
  llvm::Value *emitIntlCall(CallExpr *call);

private:
  Lowering &lowering_;
};

} // namespace tz
