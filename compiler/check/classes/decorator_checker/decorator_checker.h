#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class DecoratorChecker {
public:
  explicit DecoratorChecker(Checker &owner);

  const Type *methodDescriptorType(const Type *args, const Type *ret);
  void decorateClass(ClassDecl *cls);
  void decorate(ClassDecl *cls, ClassField *field, FunctionDecl *method,
                llvm::ArrayRef<Checker::DecoratorUse> uses);
  bool decoratorParams(Expr *callee, std::vector<const Type *> &params, size_t &required);
  void checkDecoratedNames(ClassDecl *cls);

private:
  static std::string decoratorName(const Expr *expr);
  static bool isPlainReference(const Expr *expr);
  static bool decoratedParams(const FunctionDecl *fn);

  Checker &checker;
};

}
