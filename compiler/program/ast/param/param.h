#pragma once

#include "compiler/program/ast/access/access.h"
#include "compiler/program/ast/decorator/decorator.h"
#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/type_ref/type_ref.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"

namespace tz {

class Type;

struct Param {
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::optional<TypeRef> type;
  bool optional = false;
  bool rest = false;
  Expr *defaultValue = nullptr;
  bool property = false;
  bool readonly = false;
  Access access = Access::Public;
  llvm::StringRef pattern;
  std::vector<Decorator> decorators;
  llvm::StringRef label() const { return pattern.empty() ? name : pattern; }
  const Type *declaredType = nullptr;
  VarSymbol *sym = nullptr;
};

}
