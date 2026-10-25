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
  VarSymbol *sym = nullptr; // set by Checker
  // A hidden variable holding the array a pattern destructures (see Parser),
  // or with `objectPattern` the object.
  bool destructured = false;
  bool objectPattern = false;
  // Set by Checker for `const f = genericFunction`: f is another name for the
  // function, not a variable, and sym stays null.
  bool alias = false;
  // An array pattern: how many elements it names, and whether it ends with
  // `...rest`. Destructuring an iterator takes only those from it.
  size_t patternLength = 0;
  bool patternRest = false;
};

} // namespace tz
