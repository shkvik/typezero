#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Classes: their fields and methods, the variants of overloaded methods.
class ClassDeclarer {
public:
  explicit ClassDeclarer(Sema &sema);

  // Classes (sema_class.cpp). A class is a type that is only itself; its
  // methods are functions that take the object as `this`. Classes are
  // declared with the types of their file, get their fields and the
  // signatures of their methods once types are known, and their bodies are
  // checked after top-level code, as functions are.
  void declareClass(ClassDecl *cls);
  void resolveClass(ClassDecl *cls);
  // The types of the fields that come from their initializers, found by
  // checking those (the method `init`); then the type has its fields.
  void completeClass(ClassDecl *cls);
  // FuncSymbols for the methods and constructors; and the reports about
  // variants of one name that a call cannot tell apart.
  void declareMethods(ClassDecl *cls);
  // A method whose parameters and result come from the function type
  // `declared`, where they are not written: an arrow in a field with a type,
  // or a method of a literal that goes where an interface is expected.
  void declareTyped(FunctionDecl *fn, const Type *declared);
  void checkVariants(ClassDecl *cls);
  // Reports variants `a` and `b` (declared after a) that no call can tell
  // apart.
  void checkVariantPair(FunctionDecl *a, FunctionDecl *b);
  // The class a type is an instance of, or null.
  static ClassDecl *classOf(const Type *type);
  // The methods of `cls` called `name`: its variants.
  static std::vector<FunctionDecl *> methodsNamed(const ClassDecl *cls, llvm::StringRef name,
                                                  bool isStatic);

private:
  Sema &sema_;
  llvm::SmallPtrSet<const ClassDecl *, 2> completing_;
};

} // namespace tz
