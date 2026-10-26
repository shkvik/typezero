#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/generics/inference/inference.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

namespace tz {

bool mentionsAnyOf(const Type *type, llvm::ArrayRef<const Type *> params,
                   llvm::SmallPtrSetImpl<const Type *> &seen);

std::string constraintSpelling(const Type *param, const TypeParamDecl *decl = nullptr);

}
