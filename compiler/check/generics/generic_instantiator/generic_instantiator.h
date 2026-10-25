#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The copies of generic functions made for their type arguments.
class GenericInstantiator {
public:
  explicit GenericInstantiator(Checker &checker);

  // A generic function as a value: its type arguments come from the
  // function type expected there, or are written (`first<int>`).
  Expr *checkGenericValue(NameExpr *name, FuncSymbol *generic, const Checker::Signature *expected);
  // Whether `ret`, the return type of `generic`, holds the type of an object
  // literal with methods, which each instance makes anew: such a result is
  // not supported yet (reported once).
  bool returnsOwnClass(const FuncSymbol *generic, const Type *ret);
  // The instance of `generic` for `args` (not mentioning type parameters).
  // `exact`, by parameter, the classes known when compiling that the call
  // gives where `Constructor<T>` is taken (null where none): the copy takes
  // each as the class it is (see exactClassArgs).
  FuncSymbol *instantiate(FuncSymbol *generic, llvm::ArrayRef<const Type *> args, llvm::SMLoc loc,
                          llvm::ArrayRef<const Type *> exact = {});
  // A class known when compiling, `typeof C`, given to a generic function
  // where it takes `Constructor<T>`: its copy for C takes C itself, so that
  // `new cls()` and `x instanceof cls` in it are those of C, without
  // reflection (docs/decorators.md, plan step 5). The arguments become the
  // classes; the types they are, by parameter.
  std::vector<const Type *> exactClassArgs(CallExpr *call, const FuncSymbol *generic);
  // A copy of `generic` with its type parameters standing for `args`: the
  // signature from the generic one, the body parsed again.
  FunctionDecl *instanceDecl(FuncSymbol *generic, llvm::ArrayRef<const Type *> args,
                             llvm::ArrayRef<const Type *> exact = {});
  void checkInstance(Checker::Instance *instance);
  // Reports an instance whose body has an error at the call that needs it.
  void reportInstance(Checker::Instance *instance, llvm::SMLoc callLoc,
                      llvm::ArrayRef<const Type *> concrete);
  void checkInstances();

private:
  Checker &checker_;
  // Generics: the instances, by generic function and representations; those
  // waiting for their bodies to be checked; the one being checked.
  std::map<std::pair<const FuncSymbol *, std::vector<const Type *>>, Checker::Instance *>
      instances_;
  // Generic functions reported by returnsOwnClass.
  llvm::SmallPtrSet<const FuncSymbol *, 2> ownClassReturns_;
};

} // namespace tz
