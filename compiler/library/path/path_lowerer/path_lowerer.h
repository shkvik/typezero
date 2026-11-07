#pragma once

#include "compiler/lower/lowering/lowering.h"
#include "compiler/lower/objects/object_maker/object_maker.h"

namespace tz {

class PathLowerer {
public:
  explicit PathLowerer(Lowering &owner);

  llvm::Value *emitPathCall(CallExpr *call);

private:
  static const std::vector<MakerFields> &pathObjects();

  Lowering &lowering;
};

}
