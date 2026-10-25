#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Callbacks: the functions given to calls and the array methods that take them.
class CallbackChecker {
public:
  explicit CallbackChecker(Sema &sema);

  // An argument where a function of the signature is expected. `primary` is
  // the number of arguments a built-in callback is meant to use; a function
  // passed by name must not take the others (the index, the array) into
  // optional parameters, as parseInt would take the index as its radix.
  Expr *checkFunctionArg(Expr *arg, const Sema::Signature &expected, unsigned primary,
                         llvm::StringRef method);
  // `expected` is the type wanted of the result: an array of objects that
  // map's callback returns, `xs.map(x => ({ x, y: 0 }))`, gives the objects
  // their type.
  Expr *checkCallbackMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  // Whether an arrow given to filter returns true only for a first argument
  // that holds a value: `x => x != undefined`, `x => !!x && x.length > 0`.
  bool keepsDefined(const Expr *callback) const;

private:
  Sema &sema_;
};

} // namespace tz
