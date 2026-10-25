#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The bodies of classes, this, and the fields a constructor must set.
class ClassChecker {
public:
  explicit ClassChecker(Checker &checker);

  void checkClassBodies(ClassDecl *cls);
  // The declaration in top-level code: the static fields get their values.
  void checkClassStmt(ClassDecl *cls);
  static bool hasDecorators(const ClassDecl *cls);
  // `this`, the object of the method being checked (arrows see the one of
  // the method around them), a constant that checkFunctionBody declares.
  Expr *checkThis(NameExpr *name);
  VarSymbol *declareThis(FunctionDecl *fn);
  // The rules for `this` while the fields are set: in a constructor (or the
  // initializers), until every field has a value, `this` may only be used to
  // set fields and to read those set already.
  bool thisReady(const Checker::Flow &flow) const;
  std::string firstUnset(const Checker::Flow &flow) const;
  // Whether `expr` is the `this` of the constructor being checked, used
  // directly in it.
  bool isInitThis(const Expr *expr) const;
  void checkFieldsSet(llvm::SMLoc loc);

private:
  Checker &checker_;
};

} // namespace tz
