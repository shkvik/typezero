#pragma once

#include "compiler/program/ast/stmt/stmt.h"
#include "compiler/program/ast/type_param_decl/type_param_decl.h"
#include "compiler/program/ast/type_ref/type_ref.h"

namespace tz {

class Type;

struct TypeAliasDecl : Stmt {
  TypeAliasDecl(llvm::SMLoc loc, llvm::StringRef name, TypeRef type)
      : Stmt(Kind::TypeAlias, loc), name(name), type(std::move(type)) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::TypeAlias; }

  llvm::StringRef name;
  std::vector<TypeParamDecl> typeParams;
  TypeRef type;
  std::vector<TypeRef> bases;
  const Type *resolved = nullptr;
  bool resolving = false;
  Type *self = nullptr;
  bool recursive = false;
};

}
