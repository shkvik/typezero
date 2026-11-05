#pragma once

#include "compiler/program/ast/decorator/decorator.h"
#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/param/param_form.h"
#include "compiler/program/ast/type_ref/type_ref.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"
#include <optional>

namespace tz {

class Type;

struct Param {
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::optional<TypeRef> type;
  ParamForm form = RequiredParam{};
  std::optional<ParamProperty> property;
  llvm::StringRef pattern;
  std::vector<Decorator> decorators;
  llvm::StringRef label() const { return pattern.empty() ? name : pattern; }
  const Type *declaredType = nullptr;
  VarSymbol *sym = nullptr;

  bool optional() const;
  bool rest() const;
  Expr *defaultValue() const;
};

}
