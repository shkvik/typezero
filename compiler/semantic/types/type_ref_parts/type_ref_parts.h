#pragma once

#include "compiler/program/ast/type_ref/type_ref.h"
#include "llvm/ADT/ArrayRef.h"
#include <vector>

namespace tz {

class TypeRefParts {
public:
  static std::vector<const TypeRef *> collect(const TypeRef &ref);
  static std::vector<const TypeRef *> collect(const TypeRef &ref,
                                              llvm::ArrayRef<TypeRefKind> route);
};

}
