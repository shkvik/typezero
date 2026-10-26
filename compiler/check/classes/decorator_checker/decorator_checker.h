#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class DecoratorChecker {
public:
  explicit DecoratorChecker(Checker &checker);

  const Type *methodDescriptorType(const Type *args, const Type *ret);
  void decorateClass(ClassDecl *cls);
  void decorate(ClassDecl *cls, ClassField *field, FunctionDecl *method,
                llvm::ArrayRef<Checker::DecoratorUse> uses);
  bool decoratorParams(Expr *callee, std::vector<const Type *> &params);
  void checkDecoratedNames(ClassDecl *cls);

private:
  Checker &checker_;
};

}
