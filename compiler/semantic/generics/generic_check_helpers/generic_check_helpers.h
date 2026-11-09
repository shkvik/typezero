#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

namespace tz {

class GenericCheckHelpers {
public:
  static bool mentionsAnyOf(const Type *type, llvm::ArrayRef<const Type *> params);
  static std::string constraintSpelling(const Type *param, const TypeParamDecl *decl = nullptr);

private:
  static bool isParamOf(const Type *type, llvm::ArrayRef<const Type *> params);
};

}
