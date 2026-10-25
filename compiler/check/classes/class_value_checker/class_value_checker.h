#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Classes as values: typeof C, Constructor<T>, new of a class value, mixins.
class ClassValueChecker {
public:
  explicit ClassValueChecker(Checker &checker);

  // `C.name` and the like of a class as a value, whose object is checked.
  Expr *checkClassRefMember(MemberExpr *member);
  // Classes as values (sema_classvalue.cpp, docs/decorators.md sections 5
  // and 6). `typeof C` is the class C itself, known when compiling: `new`
  // of it makes a C, and its static members are C's. `Constructor<T>` is
  // some class whose objects are T, known only when the program runs: `new`
  // of it goes through the description of the class (see reflection). A
  // class expression makes a class when Checker checks it, once for each copy
  // of a generic function that has it; such a function is a template, whose
  // body is checked only in its copies (see isTemplate).
  //
  // What objects a class as a value makes: those of its class, or T of a
  // `Constructor<T>`; null for a class of JavaScript (`Number`).
  static const Type *constructedBy(const Type *classValue);
  // `typeof C` of the class the name C means here (see finalClass).
  const Type *classValueType(ClassDecl *cls);
  // The class that a decorator gave in place of `cls`, once the
  // declaration of cls has run; cls itself otherwise.
  static ClassDecl *finalClass(ClassDecl *cls);
  // `Constructor<T>` in an annotation, and `ConstructorArgs`: the list of
  // the parameters of the constructor of the class that declaringClass_
  // extends.
  const Type *resolveConstructorType(const TypeRef &ref);
  const Type *constructorArgs(llvm::SMLoc loc);
  // `new v(...)` of a class as a value, `expr->callee`.
  Expr *checkValueNew(NewExpr *expr, const Type *expected);
  // The name of a class used as a value: `typeof C`.
  Expr *checkClassValue(NameExpr *name, ClassDecl *cls);
  // A new, checked expression of the class `cls` as a value.
  Expr *classValueAt(ClassDecl *cls, llvm::SMLoc loc);

private:
  Checker &checker_;
};

} // namespace tz
