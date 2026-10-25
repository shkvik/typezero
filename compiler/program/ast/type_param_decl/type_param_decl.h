#pragma once

#include "compiler/program/ast/type_ref/type_ref.h"

namespace tz {

// `T extends number = int`: a type parameter of a generic function or type
// alias.
struct TypeParamDecl {
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::optional<TypeRef> constraint;
  std::optional<TypeRef> defaultType;
};

} // namespace tz
