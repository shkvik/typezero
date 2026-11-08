#pragma once

#include "compiler/lower/lowering/lowering.h"
#include "compiler/lower/objects/object_maker/object_maker.h"

namespace tz {

class OsLowerer {
public:
  explicit OsLowerer(Lowering &owner);

  llvm::Value *emitOsCall(CallExpr *call);

private:
  static const std::vector<MakerFields> &osObjects();

  Lowering &lowering;
};

}
