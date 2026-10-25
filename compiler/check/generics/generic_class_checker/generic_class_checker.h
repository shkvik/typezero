#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Generic classes (docs/generics.md, section 5): their type parameters and new.
class GenericClassChecker {
public:
  explicit GenericClassChecker(Sema &sema);

  // Generic classes (sema_generic_class.cpp, docs/generics.md, section 5).
  // The bodies of a generic class are checked once, with its type
  // parameters opaque, as those of a generic function are. Each list of
  // type arguments makes an instance: a class of its own, parsed again from
  // the text of the generic class, whose fields and signatures are those of
  // the generic class with the types substituted, and whose bodies are
  // checked with those types, as copies of generic functions are (an error
  // there is reported where the instance is first used). CodeGen emits the
  // instances as classes; of the generic class itself, only its static
  // members, which all instances share.
  //
  // The opaque type parameters of a generic class, declared once.
  void declareClassTypeParams(ClassDecl *cls);
  // Makes the type parameters of a class (for an instance, the types they
  // stand for) visible by name in the innermost type scope.
  void bindClassTypeParams(const ClassDecl *cls);
  // `type` of the generic class of `inst` with the types of the instance.
  const Type *instanceType(const ClassDecl *inst, const Type *type);
  // Checks the bodies of an instance, as checkInstance does those of a copy
  // of a generic function.
  void checkClassInstance(Sema::Instance *instance);
  void reportClassInstance(Sema::Instance *instance, llvm::SMLoc loc);
  // `new Box(1)`: the type arguments from those of the constructor, and from
  // the type expected, as for a call of a generic function.
  Expr *checkGenericNew(NewExpr *expr, ClassDecl *generic, const Type *expected);

private:
  Sema &sema_;
  // The functions `new` of a generic class infers its type arguments with:
  // one for each constructor it takes (or one for none).
  std::map<std::pair<const ClassDecl *, const FunctionDecl *>, FuncSymbol *> constructorFunctions_;
};

} // namespace tz
