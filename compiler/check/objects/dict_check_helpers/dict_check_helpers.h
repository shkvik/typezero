#pragma once

#include "compiler/check/checker/checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

std::string dictText(const Expr *expr, const Type *type);

}
