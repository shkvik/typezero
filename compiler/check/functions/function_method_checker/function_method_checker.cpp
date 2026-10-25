#include "compiler/check/functions/function_method_checker/function_method_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/generics/arg_list_checker/arg_list_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

FunctionMethodChecker::FunctionMethodChecker(Sema &sema) : sema_(sema) {}

// `f.call(obj, a, b)` and `f.apply(obj, args)` of a function with a `this`
// parameter: a call of the function value with obj first. apply takes the
// arguments as the list of parameters the function ends with.
Expr *FunctionMethodChecker::checkThisCall(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
  };
  llvm::StringRef name = member->name;
  if (name != "call" && name != "apply") {
    sema_.diags_.error(member->loc(),
                 name == "bind" ? "'bind' is not supported: pass the 'this' with every call, "
                                  "'f.call(obj, ...)'"
                                : "a function has no property '" + name.str() +
                                      "'; call it with '.call(obj, ...)' or '.apply(obj, args)'");
    checkArgsLoosely();
    return call;
  }
  if (call->args.empty()) {
    sema_.diags_.error(member->loc(), "'" + name + "' takes the 'this' of the call first, e.g. 'f." +
                                    name + "(this" + (name == "apply" ? ", args)'" : ", ...)'"));
    return call;
  }
  if (name == "apply") {
    if (call->args.size() != 2 || call->hasSpread()) {
      sema_.diags_.error(member->loc(),
                   "'apply' takes two arguments: the 'this' of the call and the list "
                   "of the arguments, e.g. 'f.apply(this, args)'");
      checkArgsLoosely();
      return call;
    }
    // As `f(obj, ...args)` would pass the list: an argument list, or a
    // tuple in a variable, element by element (see expandListSpreads).
    call->spread = {false, true};
    sema_.argListChecker().expandListSpreads(call->args, call->spread);
  }
  call->callee = member->object;
  call->type = type->ret();

  return sema_.argumentChecker().checkArguments(call, type->params(), type->minArgs(), type->hasRest(), member->loc());
}

Expr *FunctionMethodChecker::checkListMethod(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (member->name != "join") {
    sema_.diags_.error(member->loc(), "a list of parameters has no method '" + member->name +
                                    "' here; it has 'length' and 'join', and goes on with "
                                    "'...args'");
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  // Each copy checks that its elements can be joined (checkTupleJoin).
  if (sema_.callChecker().rejectSpread(call) || !sema_.argumentChecker().checkArity(call, member->loc(), 0, 1)) {
    return call;
  }
  if (!call->args.empty()) {
    call->args[0] = sema_.argumentChecker().checkArg(call->args[0], Type::string());
  }
  call->type = Type::string();

  return call;
}

} // namespace tz
