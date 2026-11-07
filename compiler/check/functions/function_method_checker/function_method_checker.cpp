#include "compiler/check/functions/function_method_checker/function_method_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/generics/arg_list_checker/arg_list_checker.h"
#include "llvm/ADT/STLExtras.h"


namespace tz {

FunctionMethodChecker::FunctionMethodChecker(Checker &owner)
    : checker(owner) {}

Expr *FunctionMethodChecker::checkThisCall(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
  };
  llvm::StringRef name = member->name;
  if (name != "call" && name != "apply") {
    this->checker.diags_.error(
        member->loc(), name == "bind"
                           ? "'bind' is not supported: pass the 'this' with every call, "
                             "'f.call(obj, ...)'"
                           : "a function has no property '" + name.str() +
                                 "'; call it with '.call(obj, ...)' or '.apply(obj, args)'");
    checkArgsLoosely();
    return call;
  }
  if (call->args.empty()) {
    this->checker.diags_.error(member->loc(),
                               "'" + name + "' takes the 'this' of the call first, e.g. 'f." +
                                   name + "(this" + (name == "apply" ? ", args)'" : ", ...)'"));
    return call;
  }
  if (name == "apply") {
    if (call->args.size() != 2 || call->hasSpread()) {
      this->checker.diags_.error(member->loc(),
                                 "'apply' takes two arguments: the 'this' of the call and the list "
                                 "of the arguments, e.g. 'f.apply(this, args)'");
      checkArgsLoosely();
      return call;
    }
    call->spread = {false, true};
    this->checker.argListChecker().expandListSpreads(call->args, call->spread);
  }
  call->callee = member->object;
  call->type = type->ret();

  return this->checker.argumentChecker().checkArguments(call, type->params(), type->minArgs(),
                                                        type->hasRest(), member->loc());
}

Expr *FunctionMethodChecker::checkListMethod(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (member->name != "join") {
    this->checker.diags_.error(member->loc(),
                               "a list of parameters has no method '" + member->name +
                                   "' here; it has 'length' and 'join', and goes on with "
                                   "'...args'");
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
    return call;
  }
  bool rejected = this->checker.callChecker().rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->checker.argumentChecker().checkArity(call, 0, 1);
  if (arity.isErr()) {
    this->checker.diags_.error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  if (!call->args.empty()) {
    call->args[0] = this->checker.argumentChecker().checkArg(call->args[0], Type::string());
  }
  call->type = Type::string();

  return call;
}

}
