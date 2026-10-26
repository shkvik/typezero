#pragma once

#include "compiler/program/ast/type_ref/type_ref.h"

namespace tz {

struct TypeParamDecl {
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::optional<TypeRef> constraint;
  std::optional<TypeRef> defaultType;
};

}
