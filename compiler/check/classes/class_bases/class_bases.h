#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// extends and implements: the bases of classes and the fields of interfaces.
class ClassBases {
public:
  explicit ClassBases(Sema &sema);

  // Inheritance and interfaces (sema_inherit.cpp). A class extends one class
  // and implements interfaces; an interface extends interfaces. Those are
  // the only subtypes (docs/objects.md, section 2): a value of a type is one
  // of each type up its chain. A call of a method that some class down the
  // chain overrides goes through the method table of the object's class;
  // any other call is direct.
  //
  // `extends` and `implements` of a class, as it is resolved.
  void resolveClassBases(ClassDecl *cls);
  // The fields `interface X extends A, B` gets from A and B, followed by its
  // own; the bases go to `bases`.
  std::vector<Field> interfaceFields(TypeAliasDecl *alias, std::vector<const Type *> &bases,
                                     std::vector<Overload> &overloads);
  // Records that values of `type` are values of `base` too.
  void addBase(const Type *type, const Type *base);
  // A field of the instances of `cls`, its own or of a class up its chain
  // (the nearest declaration), and the class that declares it.
  static const ClassField *findField(const ClassDecl *cls, llvm::StringRef name,
                                     const ClassDecl **owner = nullptr);

private:
  Sema &sema_;
};

} // namespace tz
