#pragma once

#include "compiler/check/checker/checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

namespace tz {

void distribute(const Type *type, std::vector<const Type *> &out);

}
