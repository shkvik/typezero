#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/type_ref/type_ref.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"

namespace tz {

struct VarDeclarator {
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::optional<TypeRef> typeAnn;
  Expr *init = nullptr;
  VarSymbol *sym = nullptr;
  bool destructured = false;
  bool objectPattern = false;
  bool alias = false;
  size_t patternLength = 0;
  bool patternRest = false;
};

}
