#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class GenericClassChecker {
public:
  explicit GenericClassChecker(Checker &checker);

  void declareClassTypeParams(ClassDecl *cls);
  void bindClassTypeParams(const ClassDecl *cls);
  const Type *instanceType(const ClassDecl *inst, const Type *type);
  void checkClassInstance(Checker::Instance *instance);
  void reportClassInstance(Checker::Instance *instance, llvm::SMLoc loc);
  Expr *checkGenericNew(NewExpr *expr, ClassDecl *generic, const Type *expected);

private:
  Checker &checker_;
  std::map<std::pair<const ClassDecl *, const FunctionDecl *>, FuncSymbol *> constructorFunctions_;
};

}
