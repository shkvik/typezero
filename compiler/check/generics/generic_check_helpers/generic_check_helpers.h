#pragma once

#include "compiler/check/sema/sema.h"
#include "compiler/check/generics/inference/inference.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

namespace tz {

// Functions that several classes made from sema_generic.cpp use.

bool mentionsAnyOf(const Type *type, llvm::ArrayRef<const Type *> params,
                   llvm::SmallPtrSetImpl<const Type *> &seen);

// `T extends string | int`: "string | int". `number`, as written, rather
// than the double it is.
std::string constraintSpelling(const Type *param, const TypeParamDecl *decl = nullptr);

} // namespace tz
