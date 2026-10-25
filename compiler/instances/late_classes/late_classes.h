#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The classes of class expressions, made once the function they are in is known.
class LateClasses {
public:
  explicit LateClasses(Checker &checker);

  // `class extends Base { ... }` as an expression: the class it makes.
  Expr *checkClassExpr(ClassExpr *expr, const Type *expected);
  // Makes the class of a class expression, or of a declaration whose base
  // is a value (`class A extends Mixin(B)`): its base, fields, methods and
  // place among the classes; its bodies are checked with the others at the
  // end (lateClasses_).
  void makeLateClass(ClassDecl *cls);
  // The static fields of `cls` get their values where its declaration (or
  // expression) is: checked there, in order.
  void checkStaticFields(ClassDecl *cls);
  // The bodies of the classes made while code was checked.
  void checkLateClasses();
  // Methods that a class made later may override (a class expression may
  // extend any class): a call of one goes through the method table.
  void markLateOverrides(SourceFile &program);
  void markLateOverrides(ClassDecl *cls);
  // The type parameters of the copy of a generic function whose body has
  // the class expression that made `cls`.
  void bindOuterTypeParams(const ClassDecl *cls);
  // Whether `generic`, a generic function, has a class expression in its
  // body: its copies make classes of their own, so only they are checked,
  // and each finds its own return type.
  bool isTemplate(const FuncSymbol *generic);
  // Whether the body of `fn` has a class expression.
  bool hasClassExpr(const FunctionDecl *fn);
  // Checks the body of a copy of a template right away, finding its return
  // type; an error is the copy's, reported where it is called.
  void checkTemplateInstance(Checker::Instance *instance);
  // A class decorator gave `replacement`, a class that extends `cls`, in
  // its place (docs/decorators.md, section 5).
  void replaceClass(ClassDecl *cls, ClassDecl *replacement, llvm::SMLoc loc);

private:
  Checker &checker_;
  llvm::StringSet<> lateMethodNames_;
  llvm::DenseMap<const FunctionDecl *, bool> templates_;
};

} // namespace tz
