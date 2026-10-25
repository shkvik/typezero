#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Members of objects.
class MemberLowerer {
public:
  explicit MemberLowerer(CodeGen &codegen);

  llvm::Value *emitMember(MemberExpr *member);

private:
  CodeGen &codegen_;
};

} // namespace tz
