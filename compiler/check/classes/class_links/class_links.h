#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ClassLinks {
public:
  explicit ClassLinks(Checker &owner);

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
  static bool sameParams(const FuncSymbol *a, const FuncSymbol *b);
  static bool takesParams(const Type *fnType, const FuncSymbol *sym);
  static const char *accessName(Access access);
  static bool isArrowMethod(const FunctionDecl *fn);
  static bool declaresReturn(const FunctionDecl *fn);
  static void interfacesOf(const ClassDecl *cls, std::vector<const Type *> &out);

  Checker &checker;
};

}
