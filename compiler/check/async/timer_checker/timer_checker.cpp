#include "compiler/check/async/timer_checker/timer_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/functions/function_value_checker/function_value_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/prelude/prelude.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/fs/fs_checker/fs_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

constexpr int kTimerReceiver = 16;

}

TimerChecker::TimerChecker(Checker &owner)
    : checker(owner) {}

Expr *TimerChecker::checkTaskCallback(Expr *arg, llvm::StringRef fn) {
  Checker::Signature sig;
  sig.ret = Type::voidTy();
  sig.voidResult = true;
  sig.asyncVoid = true;
  const Type *task = Type::function({}, 0, false, Type::voidTy());
  if (isa<FunctionExpr>(arg)) {
    arg = this->checker.closureChecker().checkFunctionExpr(cast<FunctionExpr>(arg), &sig);
    return arg;
  }
  arg = this->checker.exprChecker().checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (!type->isFunction()) {
    this->checker.diags_.error(arg->loc(),
                               "'" + fn + "' needs a function, found '" + type->name() + "'");
    return arg;
  }
  if (type == task) {
    return arg;
  }
  if (type->minArgs() == 0 && !type->hasRest()) {
    if (Expr *adapted = this->checker.functionValueChecker().adaptFunction(arg, task)) {
      return adapted;
    }
  }
  this->checker.diags_.error(
      arg->loc(), "'" + fn + "' calls its callback with nothing, but this function is '" +
                      type->name() + "'; pass an arrow that calls it, e.g. '() => f()'");

  return arg;
}

bool TimerChecker::rearms(const Expr *callback) const {
  const FunctionDecl *current = this->checker.currentDecl();
  if (!current || !current->sym) {
    return false;
  }
  while (auto *conv = dyn_cast<CastExpr>(callback)) {
    if (!conv->isImplicit()) {
      break;
    }
    callback = conv->operand;
  }
  if (auto *member = dyn_cast<MemberExpr>(callback)) {
    return member->method() == current->sym;
  }
  auto *fe = dyn_cast<FunctionExpr>(callback);
  if (!fe || !fe->fn->body || fe->fn->body->body.size() != 1) {
    return false;
  }
  const Stmt *only = fe->fn->body->body.front();
  const Expr *value = nullptr;
  if (auto *s = dyn_cast<ExprStmt>(only)) {
    value = s->expr;
  } else if (auto *r = dyn_cast<ReturnStmt>(only)) {
    value = r->value;
  }
  auto *call = dyn_cast_or_null<CallExpr>(value);

  return call && call->method() == current->sym;
}

Expr *TimerChecker::checkAsyncGlobal(CallExpr *call, NameExpr *name) {
  static const llvm::StringLiteral names[] = {"setTimeout",    "setInterval",   "setImmediate",
                                              "clearTimeout",  "clearInterval", "clearImmediate",
                                              "queueMicrotask"};
  if (!llvm::any_of(names, [&](llvm::StringRef n) {
        return this->checker.callChecker().isGlobalName(name, n);
      })) {
    return nullptr;
  }
  this->checker.file_->usesAsync = true;
  call->type = Type::error();
  llvm::StringRef fn = name->name;
  if (this->checker.callChecker().rejectSpread(call)) {
    return call;
  }
  if (fn == "setTimeout" || fn == "setInterval") {
    if (call->args.size() > 2) {
      this->checker.diags_.error(call->args[2]->loc(),
                                 "arguments for the callback are not supported; pass an "
                                 "arrow that calls it with them");
      for (Expr *&arg : call->args) {
        arg = this->checker.exprChecker().checkExpr(arg);
      }
      return call;
    }
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->checker.diags_.error(name->loc(), arity.error().message);
      return call;
    }
    call->target =
        BuiltinCall{.builtin = fn == "setTimeout" ? Builtin::SetTimeout : Builtin::SetInterval};
    call->args[0] = this->checkTaskCallback(call->args[0], fn);
    if (call->args.size() > 1) {
      call->args[1] = this->checker.argumentChecker().checkAnyNumber(call->args[1]);
    }
    call->type = Type::timeout();
    if (call->args[0]->type->isError()) {
      return call;
    }
    int bits = 0;
    if (fn == "setInterval") {
      if (!this->checker.emitterChecker().classifyListener(call->args[0], nullptr, bits, true)) {
        return call;
      }
    } else if (this->rearms(call->args[0])) {
      this->checker.emitterChecker().classifyListener(call->args[0], nullptr, bits, true);
      bits &= kTimerReceiver;
    }
    if (bits) {
      if (call->args.size() < 2) {
        call->args.push_back(this->checker.argumentChecker().checkAnyNumber(
            this->checker.ctx_.make<IntLitExpr>(call->loc(), uint64_t(1))));
      }
      call->args.push_back(this->checker.exprChecker().checkExpr(
          this->checker.ctx_.make<IntLitExpr>(call->loc(), uint64_t(bits)), Type::intTy()));
    }
    return call;
  }
  if (fn == "setImmediate" || fn == "queueMicrotask") {
    if (call->args.size() > 1 && fn == "setImmediate") {
      this->checker.diags_.error(call->args[1]->loc(),
                                 "arguments for the callback are not supported; pass an "
                                 "arrow that calls it with them");
      for (Expr *&arg : call->args) {
        arg = this->checker.exprChecker().checkExpr(arg);
      }
      return call;
    }
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->checker.diags_.error(name->loc(), arity.error().message);
      return call;
    }
    call->target = BuiltinCall{.builtin = fn == "setImmediate" ? Builtin::SetImmediate
                                                               : Builtin::QueueMicrotask};
    call->args[0] = this->checkTaskCallback(call->args[0], fn);
    call->type = fn == "setImmediate" ? Type::immediate() : Type::voidTy();
    return call;
  }
  ArgumentChecker::ResultArityMatches arity =
      this->checker.argumentChecker().checkArity(call, 0, 1);
  if (arity.isErr()) {
    this->checker.diags_.error(name->loc(), arity.error().message);
    return call;
  }
  bool immediate = fn == "clearImmediate";
  call->target =
      BuiltinCall{.builtin = immediate ? Builtin::ClearImmediate : Builtin::ClearTimeout};
  if (call->args.empty()) {
    auto *none = this->checker.ctx_.make<UndefinedExpr>(call->loc());
    none->type = Type::undefinedTy();
    call->args.push_back(none);
  }
  call->args[0] = this->checker.argumentChecker().checkArg(
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
  if (this->checker.callChecker().isGlobalName(callee, "setTimeout") ||
      this->checker.callChecker().isGlobalName(callee, "setInterval")) {
    return Type::timeout();
  }
  if (this->checker.callChecker().isGlobalName(callee, "setImmediate")) {
    return Type::immediate();
  }

  return nullptr;
}

Expr *TimerChecker::checkNextTick(CallExpr *call, MemberExpr *member) {
  this->checker.file_->usesAsync = true;
  call->type = Type::error();
  if (call->args.size() > 1) {
    this->checker.diags_.error(call->args[1]->loc(),
                               "arguments for the callback are not supported; pass an "
                               "arrow that calls it with them");
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
    return call;
  }
  ArgumentChecker::ResultArityMatches arity =
      this->checker.argumentChecker().checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->checker.diags_.error(member->loc(), arity.error().message);
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::NextTick};
  call->args[0] = this->checkTaskCallback(call->args[0], "process.nextTick");
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
    this->checker.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                                  type->name() +
                                                  "'; it has 'ref', 'unref', 'hasRef'" +
                                                  (type->isImmediate() ? "" : " and 'refresh'"));
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
    return call;
  }
  bool rejected = this->checker.callChecker().rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->checker.argumentChecker().checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->checker.diags_.error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  call->target = BuiltinCall{.builtin = method->builtin};
  call->type = method->builtin == Builtin::TimerHasRef ? Type::boolTy() : type;

  return call;
}

Expr *TimerChecker::checkTimersPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                                            const Type *expected) {
  this->checker.file_->usesAsync = true;
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }

    return call;
  };
  if (fn == "scheduler") {
    this->checker.diags_.error(loc,
                               "'scheduler' is used through its functions: 'scheduler.wait(ms)' or "
                               "'scheduler.yield()'");
    return loosely();
  }
  if (this->checker.callChecker().rejectSpread(call)) {
    return call;
  }
  bool immediate = fn == "setImmediate";
  bool interval = fn == "setInterval";
  size_t valueAt = immediate ? 0 : 1;
  ArgumentChecker::ResultArityMatches arity =
      this->checker.argumentChecker().checkArity(call, 0, unsigned(valueAt + 2));
  if (arity.isErr()) {
    this->checker.diags_.error(loc, arity.error().message);
    return call;
  }
  std::vector<Expr *> args;
  if (!immediate) {
    if (call->args.empty() || isa<UndefinedExpr>(call->args[0])) {
      auto *one = this->checker.ctx_.make<FloatLitExpr>(loc, 1.0);
      one->type = Type::doubleTy();
      args.push_back(one);
    } else {
      args.push_back(this->checker.argumentChecker().checkAnyNumber(call->args[0]));
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
    given = this->checker.exprChecker().checkExpr(given, wanted);
    value = given->type;
    if (value->isError()) {
      return call;
    }
    if (value->isVoid()) {
      this->checker.diags_.error(given->loc(),
                                 "a value of type 'void' cannot be given to '" + fn + "'");
      return call;
    }
    if (value->isUndefined()) {
      value = Type::voidTy();
    } else {
      if (value->isLiteral()) {
        value = value->literalBase();
      }
      given = this->checker.typeRelations().coerce(given, value);
      args.push_back(given);
    }
  } else if (given) {
    this->checker.exprChecker().checkExpr(given);
  }
  llvm::StringMap<Expr *> options;
  if (!this->checker.fsChecker().nodeOptions(call, valueAt + 1, fn, {"ref"}, options)) {
    return call;
  }
  if (Expr *ref = options.lookup("ref")) {
    args.push_back(this->checker.argumentChecker().checkArg(ref, Type::boolTy()));
  } else {
    auto *yes = this->checker.ctx_.make<BoolLitExpr>(loc, true);
    yes->type = Type::boolTy();
    args.push_back(yes);
  }
  call->args = std::move(args);
  call->spread.clear();
  if (interval) {
    call->target = BuiltinCall{.builtin = Builtin::TimersSetInterval};
    call->type = Type::asyncIterator(value->isVoid() ? Type::undefinedTy() : value);
    return call;
  }
  call->target =
      BuiltinCall{.builtin = immediate ? Builtin::TimersSetImmediate : Builtin::TimersSetTimeout};
  call->type = Type::promise(value);

  return call;
}

Expr *TimerChecker::checkSchedulerCall(CallExpr *call, MemberExpr *member) {
  this->checker.file_->usesAsync = true;
  call->type = Type::error();
  if (member->name == "wait") {
    bool rejected = this->checker.callChecker().rejectSpread(call);
    ArgumentChecker::ResultArityMatches arity =
        rejected ? ok(call) : this->checker.argumentChecker().checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->checker.diags_.error(member->loc(), arity.error().message);
    }
    if (rejected || arity.isErr()) {
      return call;
    }
    Expr *options = call->args.size() > 1 ? call->args[1] : nullptr;
    auto *none = this->checker.ctx_.make<UndefinedExpr>(member->loc());
    call->args = {call->args[0], none};
    if (options) {
      call->args.push_back(options);
    }
    return this->checkTimersPromisesCall(call, "setTimeout", member->loc(), nullptr);
  }
  if (member->name == "yield") {
    bool rejected = this->checker.callChecker().rejectSpread(call);
    ArgumentChecker::ResultArityMatches arity =
        rejected ? ok(call) : this->checker.argumentChecker().checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->checker.diags_.error(member->loc(), arity.error().message);
    }
    if (rejected || arity.isErr()) {
      return call;
    }
    return this->checkTimersPromisesCall(call, "setImmediate", member->loc(), nullptr);
  }
  this->checker.diags_.error(member->loc(),
                             "'scheduler." + member->name +
                                 "' does not exist; scheduler has 'wait' and 'yield'");
  for (Expr *&arg : call->args) {
    arg = this->checker.exprChecker().checkExpr(arg);
  }

  return call;
}

}
