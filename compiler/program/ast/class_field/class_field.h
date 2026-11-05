#pragma once

#include "compiler/program/ast/access/access.h"
#include "compiler/program/ast/class_field/field_form.h"
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
  FieldForm form = InstanceField{};
  FieldPresence presence = FieldPresence::REQUIRED;
  bool readonly = false;
  bool isOverride = false;
  Access access = Access::Public;
  const Type *resolved = nullptr;
  bool redeclared = false;
  std::vector<Decorator> decorators;
  bool accessor = false;

  bool isStatic() const;
  bool parameter() const;
  FunctionDecl *method() const;
  VarSymbol *var() const;
  bool optional() const;
  bool definite() const;
};

}
