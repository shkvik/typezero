#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Type annotations to types.
class TypeResolver {
public:
  explicit TypeResolver(Checker &checker);

  const Type *resolveType(const TypeRef &ref, bool allowVoid);
  const Type *resolveTypeInner(const TypeRef &ref);
  // The fields of an object type written in the source; with `overloads`,
  // its methods with several signatures go there (only an interface has
  // them).
  std::vector<Field> resolveFields(const TypeRef &ref, std::vector<Overload> *overloads = nullptr);

private:
  Checker &checker_;
};

} // namespace tz
