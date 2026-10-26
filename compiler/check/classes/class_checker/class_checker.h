#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ClassChecker {
public:
  explicit ClassChecker(Checker &checker);

  void checkClassBodies(ClassDecl *cls);
  void checkClassStmt(ClassDecl *cls);
  static bool hasDecorators(const ClassDecl *cls);
  Expr *checkThis(NameExpr *name);
  VarSymbol *declareThis(FunctionDecl *fn);
  bool thisReady(const Checker::Flow &flow) const;
  std::string firstUnset(const Checker::Flow &flow) const;
  bool isInitThis(const Expr *expr) const;
  void checkFieldsSet(llvm::SMLoc loc);

private:
  Checker &checker_;
};

}
