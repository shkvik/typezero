#include "compiler/check/async/timer_checker/timer_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/functions/function_value_checker/function_value_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/prelude/prelude.h"
#include "compiler/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/library/fs/fs_checker/fs_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

TimerChecker::TimerChecker(Checker &checker) : checker_(checker) {}

Expr *TimerChecker::checkTaskCallback(Expr *arg, llvm::StringRef fn) {
  Checker::Signature sig;
  sig.ret = Type::voidTy();
  sig.voidResult = true;
  sig.asyncVoid = true;
  const Type *task = Type::function({}, 0, false, Type::voidTy());
  if (isa<FunctionExpr>(arg)) {
    arg = checker_.closureChecker().checkFunctionExpr(cast<FunctionExpr>(arg), &sig);
    return arg;
  }
  arg = checker_.exprChecker().checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (!type->isFunction()) {
    checker_.diags_.error(arg->loc(),
                          "'" + fn + "' needs a function, found '" + type->name() + "'");
    return arg;
  }
  if (type == task) {
    return arg;
  }
  if (type->minArgs() == 0 && !type->hasRest()) {
    if (Expr *adapted = checker_.functionValueChecker().adaptFunction(arg, task)) {
      return adapted;
    }
  }
  checker_.diags_.error(arg->loc(),
                        "'" + fn + "' calls its callback with nothing, but this function is '" +
                            type->name() + "'; pass an arrow that calls it, e.g. '() => f()'");

  return arg;
}

Expr *TimerChecker::checkAsyncGlobal(CallExpr *call, NameExpr *name) {
  static const llvm::StringLiteral names[] = {"setTimeout",    "setInterval",   "setImmediate",
                                              "clearTimeout",  "clearInterval", "clearImmediate",
                                              "queueMicrotask"};
  if (!llvm::any_of(
          names, [&](llvm::StringRef n) { return checker_.callChecker().isGlobalName(name, n); })) {
    return nullptr;
  }
  checker_.file_->usesAsync = true;
  call->type = Type::error();
  llvm::StringRef fn = name->name;
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  if (fn == "setTimeout" || fn == "setInterval") {
    if (call->args.size() > 2) {
      checker_.diags_.error(call->args[2]->loc(),
                            "arguments for the callback are not supported; pass an "
                            "arrow that calls it with them");
      for (Expr *&arg : call->args) {
        arg = checker_.exprChecker().checkExpr(arg);
      }
      return call;
    }
    if (!checker_.argumentChecker().checkArity(call, name->loc(), 1, 2)) {
      return call;
    }
    call->builtin = fn == "setTimeout" ? Builtin::SetTimeout : Builtin::SetInterval;
    call->args[0] = checkTaskCallback(call->args[0], fn);
    if (call->args.size() > 1) {
      call->args[1] = checker_.argumentChecker().checkArg(call->args[1], Type::doubleTy());
    }
    call->type = Type::timeout();
    return call;
  }
  if (fn == "setImmediate" || fn == "queueMicrotask") {
    if (call->args.size() > 1 && fn == "setImmediate") {
      checker_.diags_.error(call->args[1]->loc(),
                            "arguments for the callback are not supported; pass an "
                            "arrow that calls it with them");
      for (Expr *&arg : call->args) {
        arg = checker_.exprChecker().checkExpr(arg);
      }
      return call;
    }
    if (!checker_.argumentChecker().checkArity(call, name->loc(), 1, 1)) {
      return call;
    }
    call->builtin = fn == "setImmediate" ? Builtin::SetImmediate : Builtin::QueueMicrotask;
    call->args[0] = checkTaskCallback(call->args[0], fn);
    call->type = fn == "setImmediate" ? Type::immediate() : Type::voidTy();
    return call;
  }
  if (!checker_.argumentChecker().checkArity(call, name->loc(), 0, 1)) {
    return call;
  }
  bool immediate = fn == "clearImmediate";
  call->builtin = immediate ? Builtin::ClearImmediate : Builtin::ClearTimeout;
  if (call->args.empty()) {
    auto *none = checker_.ctx_.make<UndefinedExpr>(call->loc());
    none->type = Type::undefinedTy();
    call->args.push_back(none);
  }
  call->args[0] = checker_.argumentChecker().checkArg(
      call->args[0], Type::optional(immediate ? Type::immediate() : Type::timeout()));
  call->type = Type::voidTy();

  return call;
}

const Type *TimerChecker::timerCallType(const Expr *init) const {
  auto *call = dyn_cast<CallExpr>(init);
  const Expr *callee = call ? call->callee : nullptr;
  if (!callee) {
    return nullptr;
  }
  if (checker_.callChecker().isGlobalName(callee, "setTimeout") ||
      checker_.callChecker().isGlobalName(callee, "setInterval")) {
    return Type::timeout();
  }
  if (checker_.callChecker().isGlobalName(callee, "setImmediate")) {
    return Type::immediate();
  }

  return nullptr;
}

Expr *TimerChecker::checkNextTick(CallExpr *call, MemberExpr *member) {
  checker_.file_->usesAsync = true;
  call->type = Type::error();
  if (call->args.size() > 1) {
    checker_.diags_.error(call->args[1]->loc(),
                          "arguments for the callback are not supported; pass an "
                          "arrow that calls it with them");
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
    return call;
  }
  call->builtin = Builtin::NextTick;
  call->args[0] = checkTaskCallback(call->args[0], "process.nextTick");
  call->type = Type::voidTy();

  return call;
}

Expr *TimerChecker::checkTimerMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  call->type = Type::error();
  struct Method {
    llvm::StringLiteral name;
    Builtin builtin;
  };
  static const Method methods[] = {{"ref", Builtin::TimerRef},
                                   {"unref", Builtin::TimerUnref},
                                   {"hasRef", Builtin::TimerHasRef},
                                   {"refresh", Builtin::TimerRefresh}};
  const Method *method = nullptr;
  for (const Method &m : methods) {
    if (m.name == name && (m.builtin != Builtin::TimerRefresh || !type->isImmediate())) {
      method = &m;
    }
  }
  if (!method) {
    checker_.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                             type->name() + "'; it has 'ref', 'unref', 'hasRef'" +
                                             (type->isImmediate() ? "" : " and 'refresh'"));
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
    return call;
  }
  call->builtin = method->builtin;
  call->type = method->builtin == Builtin::TimerHasRef ? Type::boolTy() : type;

  return call;
}

Expr *TimerChecker::checkTimersPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                                            const Type *expected) {
  checker_.file_->usesAsync = true;
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }

    return call;
  };
  if (fn == "scheduler") {
    checker_.diags_.error(loc, "'scheduler' is used through its functions: 'scheduler.wait(ms)' or "
                               "'scheduler.yield()'");
    return loosely();
  }
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  bool immediate = fn == "setImmediate";
  bool interval = fn == "setInterval";
  size_t valueAt = immediate ? 0 : 1;
  if (!checker_.argumentChecker().checkArity(call, loc, 0, unsigned(valueAt + 2))) {
    return call;
  }
  std::vector<Expr *> args;
  if (!immediate) {
    if (call->args.empty() || isa<UndefinedExpr>(call->args[0])) {
      auto *one = checker_.ctx_.make<FloatLitExpr>(loc, 1.0);
      one->type = Type::doubleTy();
      args.push_back(one);
    } else {
      args.push_back(checker_.argumentChecker().checkArg(call->args[0], Type::doubleTy()));
    }
  }
  const Type *value = Type::voidTy();
  Expr *given = valueAt < call->args.size() ? call->args[valueAt] : nullptr;
  if (given && !isa<UndefinedExpr>(given)) {
    const Type *wanted = nullptr;
    if (expected && expected->nonOptional()->isPromise()) {
      wanted = expected->nonOptional()->promised();
    }
    if (wanted && (wanted->isVoid() || wanted->isNever())) {
      wanted = nullptr;
    }
    given = checker_.exprChecker().checkExpr(given, wanted);
    value = given->type;
    if (value->isError()) {
      return call;
    }
    if (value->isVoid()) {
      checker_.diags_.error(given->loc(), "a value of type 'void' cannot be given to '" + fn + "'");
      return call;
    }
    if (value->isUndefined()) {
      value = Type::voidTy();
    } else {
      if (value->isLiteral()) {
        value = value->literalBase();
      }
      given = checker_.typeRelations().coerce(given, value);
      args.push_back(given);
    }
  } else if (given) {
    checker_.exprChecker().checkExpr(given);
  }
  llvm::StringMap<Expr *> options;
  if (!checker_.fsChecker().nodeOptions(call, valueAt + 1, fn, {"signal", "ref"}, options)) {
    return call;
  }
  Expr *signal = options.lookup("signal");
  bool abortable = signal && !isa<UndefinedExpr>(signal);
  args.push_back(checker_.runtimeObjectChecker().signalOption(signal, loc));
  if (Expr *ref = options.lookup("ref")) {
    args.push_back(checker_.argumentChecker().checkArg(ref, Type::boolTy()));
  } else {
    auto *yes = checker_.ctx_.make<BoolLitExpr>(loc, true);
    yes->type = Type::boolTy();
    args.push_back(yes);
  }
  call->args = std::move(args);
  call->spread.clear();
  if (interval) {
    call->builtin = Builtin::TimersSetInterval;
    call->type = Type::asyncIterator(value->isVoid() ? Type::undefinedTy() : value);
    return call;
  }
  call->builtin = immediate ? Builtin::TimersSetImmediate : Builtin::TimersSetTimeout;
  call->type = Type::promise(
      abortable ? Type::result(value, checker_.prelude().builtinType("AbortError")) : value);

  return call;
}

Expr *TimerChecker::checkSchedulerCall(CallExpr *call, MemberExpr *member) {
  checker_.file_->usesAsync = true;
  call->type = Type::error();
  if (member->name == "wait") {
    if (checker_.callChecker().rejectSpread(call) ||
        !checker_.argumentChecker().checkArity(call, member->loc(), 1, 2)) {
      return call;
    }
    Expr *options = call->args.size() > 1 ? call->args[1] : nullptr;
    auto *none = checker_.ctx_.make<UndefinedExpr>(member->loc());
    call->args = {call->args[0], none};
    if (options) {
      call->args.push_back(options);
    }
    return checkTimersPromisesCall(call, "setTimeout", member->loc(), nullptr);
  }
  if (member->name == "yield") {
    if (checker_.callChecker().rejectSpread(call) ||
        !checker_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
      return call;
    }
    return checkTimersPromisesCall(call, "setImmediate", member->loc(), nullptr);
  }
  checker_.diags_.error(member->loc(), "'scheduler." + member->name +
                                           "' does not exist; scheduler has 'wait' and 'yield'");
  for (Expr *&arg : call->args) {
    arg = checker_.exprChecker().checkExpr(arg);
  }

  return call;
}

}
