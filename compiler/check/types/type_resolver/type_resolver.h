#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TypeResolver {
public:
  explicit TypeResolver(Checker &checker);

  const Type *resolveType(const TypeRef &ref, bool allowVoid);
  const Type *resolveTypeInner(const TypeRef &ref);
  std::vector<Field> resolveFields(const TypeRef &ref, std::vector<Overload> *overloads = nullptr);

private:
  Checker &checker_;
};

}
