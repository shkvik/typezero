#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class InheritChecker {
public:
  explicit InheritChecker(Checker &owner);

  void checkAbstract(ClassDecl *cls);
  void checkImplements(ClassDecl *cls);
  Expr *checkSuperCall(CallExpr *call);
  Expr *checkSuperMethodCall(CallExpr *call, MemberExpr *member);

private:
  static bool takesParams(const Type *fnType, const FuncSymbol *sym);
  static bool isArrowMethod(const FunctionDecl *fn);
  static void interfacesOf(const ClassDecl *cls, std::vector<const Type *> &out);

  Checker &checker;
};

}
