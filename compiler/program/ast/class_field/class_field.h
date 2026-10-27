#pragma once

#include "compiler/program/ast/access/access.h"
#include "compiler/program/ast/decorator/decorator.h"
#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/function_decl/function_decl.h"
#include "compiler/program/ast/type_ref/type_ref.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"

namespace tz {

class Type;

struct ClassField {
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::optional<TypeRef> type;
  Expr *init = nullptr;
  bool optional = false;
  bool definite = false;
  bool readonly = false;
  bool isStatic = false;
  bool parameter = false;
  bool isOverride = false;
  Access access = Access::Public;
  const Type *resolved = nullptr;
  VarSymbol *var = nullptr;
  bool redeclared = false;
  FunctionDecl *method = nullptr;
  std::vector<Decorator> decorators;
  bool accessor = false;
};

}
