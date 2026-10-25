#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Type parameters: their declarations, bindings, constraints and substitution.
class TypeParams {
public:
  explicit TypeParams(Sema &sema);

  // The type parameters of a generic function or alias: new opaque types
  // with their constraints, visible by name in the innermost type scope.
  std::vector<const Type *> declareTypeParams(llvm::ArrayRef<TypeParamDecl> decls,
                                              std::vector<const Type *> *defaults);
  // Makes the names of `decls` stand for `types` in the innermost type scope.
  void bindTypeParams(llvm::ArrayRef<TypeParamDecl> decls, llvm::ArrayRef<const Type *> types);
  // `type` with `params` replaced by `args`.
  const Type *substitute(const Type *type, llvm::ArrayRef<const Type *> params,
                         llvm::ArrayRef<const Type *> args);
  // What instances of a generic function are made for: types whose values
  // look the same in memory, and to code that cannot reach their fields, map
  // to one. All objects are a pointer (released through their descriptor),
  // all functions a code and an environment.
  static const Type *representation(const Type *type);
  // Whether `arg` may stand for type parameter `param`.
  // A constraint that mentions the type parameters `params`, `K extends
  // keyof T`, is checked with the types `args` they stand for.
  bool satisfies(const Type *arg, const Type *param, llvm::ArrayRef<const Type *> params = {},
                 llvm::ArrayRef<const Type *> args = {});
  // `T extends { name: string }`: whether `arg` has at least the members of
  // the object type `constraint`, with the same types (a class may have a
  // method for one that holds a function); or is that type, or one declared
  // to be it.
  bool hasMembersOf(const Type *arg, const Type *constraint);
  // Whether the copies of `generic` are made for each type its type
  // parameter `k` stands for rather than for each representation: the body
  // reads the fields of the type (an object constraint), or its signature
  // has a generic class of it, whose instances are classes of their own.
  bool exactTypeParam(FuncSymbol *generic, size_t k);

private:
  Sema &sema_;
  // The object types substitute is rebuilding: a recursive one that no alias
  // made stays as it is.
  llvm::SmallPtrSet<const Type *, 4> substituting_;
};

} // namespace tz
