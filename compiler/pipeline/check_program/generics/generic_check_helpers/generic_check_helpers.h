#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/generics/inference/inference.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

namespace tz {

bool mentionsAnyOf(const Type *type, llvm::ArrayRef<const Type *> params,
                   llvm::SmallPtrSetImpl<const Type *> &seen);

std::string constraintSpelling(const Type *param, const TypeParamDecl *decl = nullptr);

}
