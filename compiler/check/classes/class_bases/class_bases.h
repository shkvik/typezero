#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ClassBases {
public:
  explicit ClassBases(Checker &owner);

  void resolveClassBases(ClassDecl *cls);
  std::vector<Field> interfaceFields(TypeAliasDecl *alias, std::vector<const Type *> &bases,
                                     std::vector<Overload> &overloads);
  void addBase(const Type *type, const Type *base);
  static const ClassField *findField(const ClassDecl *cls, llvm::StringRef name,
                                     const ClassDecl **owner = nullptr);

private:
  static bool isErrorClass(llvm::StringRef name);
  static const char *accessName(Access access);

  Checker &checker;
};

}
