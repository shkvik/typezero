#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class MemberLowerer {
public:
  explicit MemberLowerer(Lowering &lowering);

  llvm::Value *emitMember(MemberExpr *member);

private:
  Lowering &lowering_;
};

}
