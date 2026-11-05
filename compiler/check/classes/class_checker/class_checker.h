#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/result/result/result.h"

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
  Result<const FunctionDecl *, std::string> thisOf(const FunctionDecl *current) const;
  void reportThis(llvm::SMLoc loc, llvm::StringRef code);

  Checker &checker_;
};

}
