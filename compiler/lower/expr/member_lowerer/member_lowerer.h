#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Members of objects.
class MemberLowerer {
public:
  explicit MemberLowerer(Lowering &lowering);

  llvm::Value *emitMember(MemberExpr *member);

private:
  Lowering &lowering_;
};

} // namespace tz
