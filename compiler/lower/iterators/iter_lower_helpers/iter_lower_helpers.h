#pragma once

#include "compiler/lower/lowering/lowering.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

bool hasTry(const Stmt *stmt);

}
