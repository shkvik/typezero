#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Decorators and the order they apply in (docs/decorators.md).
class DecoratorChecker {
public:
  explicit DecoratorChecker(Sema &sema);

  // `MethodDescriptor<A, R>`: `{ value: (this: unknown, ...args: A) => R;
  // writable, enumerable, configurable: boolean }`, what a decorator of a
  // method gets (docs/decorators.md, section 2). The types that made each,
  // for substitute.
  const Type *methodDescriptorType(const Type *args, const Type *ret);
  // Applies the decorators of `cls` and of its members where its declaration
  // runs, after the static fields get their values (checkClassStmt): makes
  // and checks the statements that do it, cls->decorations.
  void decorateClass(ClassDecl *cls);
  // The decorators of one declaration of `cls`: the class (`field` and
  // `method` null), a field or a method, with those of the parameters.
  void decorate(ClassDecl *cls, ClassField *field, FunctionDecl *method,
                llvm::ArrayRef<Sema::DecoratorUse> uses);
  // The parameters of the decorator `callee`, if they are known: false for
  // a rest parameter or a callee known only when checked.
  bool decoratorParams(Expr *callee, std::vector<const Type *> &params);
  // Reports two decorated members of one name: their records would be one.
  void checkDecoratedNames(ClassDecl *cls);

private:
  Sema &sema_;
};

} // namespace tz
