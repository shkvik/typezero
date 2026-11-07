#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class IntlCallLowerer {
public:
  explicit IntlCallLowerer(Lowering &owner);

  llvm::Value *emitIntlCall(CallExpr *call);

private:
  static int optionIndex(llvm::StringRef name);

  Lowering &lowering;
};

}
