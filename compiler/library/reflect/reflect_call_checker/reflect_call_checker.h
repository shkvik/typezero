#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Reflect.get, apply and construct.
class ReflectCallChecker {
public:
  explicit ReflectCallChecker(Checker &checker);

  Expr *checkReflectArgs(Expr *args);
  // Reflection where the class is known when compiling (docs/decorators.md,
  // plan step 5). The method no class down the chain of `cls` declares
  // again, under its name, which Reflect.get(object, name) gives for an
  // object of `cls`: the function that takes its `this`, or of `several`
  // variants, the one that chooses (docs/overloads.md, section 6). Null for
  // none.
  FunctionDecl *knownReflectMethod(ClassDecl *cls, llvm::StringRef name, bool &several);
  // The method of one variant `f` surely holds, as Reflect.get of an object
  // of a class known when compiling gave it, or null.
  const FunctionDecl *heldReflectMethod(const Expr *f);
  // Reflect.apply(f, self, [args]) of such a method `fn`, with a `self` of
  // its class and arguments that the checks would let through: the call
  // itself, `self.m(args)`, as an unknown; null if it cannot be one.
  Expr *directReflectApply(CallExpr *call, const FunctionDecl *fn);
  // Metadata (docs/decorators.md, section 3): Reflect.defineMetadata and the
  // others, whose records by an object key (a decorator, a class, a
  // constant) have the type of the first one written, and by a string key
  // are unknown; and the records `design:*` the compiler writes.
  Expr *checkReflectCall(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  Checker &checker_;
};

} // namespace tz
