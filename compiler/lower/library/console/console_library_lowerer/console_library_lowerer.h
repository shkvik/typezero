#pragma once

#include "compiler/lower/library/library_lowerer/library_lowerer.h"

namespace tz {

class ConsoleLibraryLowerer : public LibraryLowerer {
public:
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

}
