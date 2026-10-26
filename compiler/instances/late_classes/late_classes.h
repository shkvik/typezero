#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class LateClasses {
public:
  explicit LateClasses(Checker &checker);

  Expr *checkClassExpr(ClassExpr *expr, const Type *expected);
  void makeLateClass(ClassDecl *cls);
  void checkStaticFields(ClassDecl *cls);
  void checkLateClasses();
  void markLateOverrides(SourceFile &program);
  void markLateOverrides(ClassDecl *cls);
  void bindOuterTypeParams(const ClassDecl *cls);
  bool isTemplate(const FuncSymbol *generic);
  bool hasClassExpr(const FunctionDecl *fn);
  void checkTemplateInstance(Checker::Instance *instance);
  void replaceClass(ClassDecl *cls, ClassDecl *replacement, llvm::SMLoc loc);

private:
  Checker &checker_;
  llvm::StringSet<> lateMethodNames_;
  llvm::DenseMap<const FunctionDecl *, bool> templates_;
};

}
