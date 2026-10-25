#pragma once

#include "compiler/program/ast/stmt/stmt.h"
#include "compiler/program/ast/type_param_decl/type_param_decl.h"
#include "compiler/program/ast/type_ref/type_ref.h"

namespace tz {

class Type;

// `type Name = T`: another name for a type, visible in the whole block that
// declares it. `interface Name { ... }` is the same for an object type.
struct TypeAliasDecl : Stmt {
  TypeAliasDecl(llvm::SMLoc loc, llvm::StringRef name, TypeRef type)
      : Stmt(Kind::TypeAlias, loc), name(name), type(std::move(type)) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::TypeAlias; }

  llvm::StringRef name;
  // `type Pair<T> = ...`: a generic alias, which names a type for each list
  // of type arguments (see Sema::instantiateAlias).
  std::vector<TypeParamDecl> typeParams;
  TypeRef type;
  // `interface User extends Named, Dated { ... }`: the interfaces whose
  // fields User has too, and which it may be used as.
  std::vector<TypeRef> bases;
  // Set by Sema.
  const Type *resolved = nullptr;
  bool resolving = false; // catches an alias that refers to itself
  // An object type, or a union of them, may refer to itself through its
  // fields: while it is resolved, they get this type (see Type::declareObject
  // and Type::declareUnion).
  Type *self = nullptr;
  bool recursive = false; // some field did
};

} // namespace tz
