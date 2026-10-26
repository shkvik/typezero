#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ClassLinks {
public:
  explicit ClassLinks(Checker &checker);

  void numberClasses();
  void buildVtable(ClassDecl *cls);
  void linkClasses();
  void linkOverrides(ClassDecl *cls);
  static std::vector<FunctionDecl *> constructorsOf(const ClassDecl *cls);
  static std::vector<FunctionDecl *> ownMethods(const ClassDecl *cls);
  static bool overridesTransitively(const FunctionDecl *fn, const FunctionDecl *base);
  static FunctionDecl *soleImplementation(const ClassDecl *cls, FunctionDecl *variant);
  const Type *inheritedReturnType(FuncSymbol *fn);
  void noteInheritedReturn(const FunctionDecl *fn);
  static const Type *commonClass(const Type *a, const Type *b);

private:
  Checker &checker_;
};

}
