#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TypeOperators {
public:
  explicit TypeOperators(Checker &checker);

  const Type *keysOf(const Type *type);
  const Type *evalTypeOp(const Type *op, llvm::ArrayRef<const Type *> operands);
  const Type *resolveTypeOp(const TypeRef &ref);

private:
  Checker &checker_;
};

}
