#pragma once

#include "compiler/check/checker/checker.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

const Type *writtenLiteral(const Expr *expr);

}
