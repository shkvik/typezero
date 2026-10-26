#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class IntlCallLowerer {
public:
  explicit IntlCallLowerer(Lowering &lowering);

  llvm::Value *emitIntlCall(CallExpr *call);

private:
  Lowering &lowering_;
};

}
