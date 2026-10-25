#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// keyof, T[K], mapped types and typeof in types.
class TypeOperators {
public:
  explicit TypeOperators(Sema &sema);

  // `keyof type`: the names of its fields, as a union of string literal
  // types; null if it is no object type.
  const Type *keysOf(const Type *type);
  // The computation `op` (Type::typeOp) with these operands: a type if they
  // are known, or the computation again.
  const Type *evalTypeOp(const Type *op, llvm::ArrayRef<const Type *> operands);
  // `keyof T`, `T[K]`, `typeof x` and mapped types in a type annotation;
  // null for any other.
  const Type *resolveTypeOp(const TypeRef &ref);

private:
  Sema &sema_;
};

} // namespace tz
