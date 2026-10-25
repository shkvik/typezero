#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Overrides, tables of methods and the numbers of classes.
class ClassLinks {
public:
  explicit ClassLinks(Sema &sema);

  // Numbers the classes for instanceof, so that a class is followed by the
  // classes down its chain; again at the end, with the instances made late.
  void numberClasses();
  // The vtable of `cls`, after that of the class it extends.
  void buildVtable(ClassDecl *cls);
  // Once every class has its methods: which method overrides which, the
  // methods that classes down the chain override, the numbers of the classes
  // for instanceof and the slots of their method tables.
  void linkClasses();
  void linkOverrides(ClassDecl *cls);
  // The constructors `new cls(...)` chooses from: its own, or those of the
  // nearest class up its chain that has any.
  static std::vector<FunctionDecl *> constructorsOf(const ClassDecl *cls);
  // The instance methods a class declares itself: its methods, and the
  // arrows in its fields that are compiled as methods.
  static std::vector<FunctionDecl *> ownMethods(const ClassDecl *cls);
  // Whether `fn` replaces `base`, directly or through other overrides.
  static bool overridesTransitively(const FunctionDecl *fn, const FunctionDecl *base);
  // The one method with a body that a call of `variant` on an object of
  // class `cls` (or of a class down its chain) may run, or null if there are
  // several (or none): then the call goes through the method table.
  static FunctionDecl *soleImplementation(const ClassDecl *cls, FunctionDecl *variant);
  // The return type of a method that overrides another one, or implements a
  // method of an interface, and declares none: the type the other one
  // returns. Null for any other function.
  const Type *inheritedReturnType(FuncSymbol *fn);
  // After an error in a return of `fn`: where its return type comes from,
  // if it overrides a method or implements one of an interface.
  void noteInheritedReturn(const FunctionDecl *fn);
  // The nearest class that both classes are, or null.
  static const Type *commonClass(const Type *a, const Type *b);

private:
  Sema &sema_;
};

} // namespace tz
