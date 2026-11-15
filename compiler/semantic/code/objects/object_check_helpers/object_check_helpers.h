#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

class ObjectCheckHelpers {
public:
  static const Expr *unnarrowed(const Expr *expr);
};

}
