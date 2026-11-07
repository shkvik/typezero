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
  bool adaptable = type->isFunction() && type != task && type->minArgs() == 0 && !type->hasRest();
  Expr *adapted =
      adaptable ? this->checker.functionValueChecker().adaptFunction(arg, task) : nullptr;
  ResultTaskCallbackOk fits = TimerChecker::taskCallback(type, task, adapted, fn);
  if (fits.isErr()) {
    this->checker.diags_.error(arg->loc(), fits.error().message);
    return arg;
  }

  return adapted ? adapted : arg;
}

TimerChecker::ResultTaskCallbackOk TimerChecker::taskCallback(const Type *type, const Type *task,
                                                              const Expr *adapted,
                                                              llvm::StringRef fn) {
  if (!type->isFunction()) {
    std::string message = ErrorTextTaskCallback::notFunction(fn, type->name());

    return err(TimerCheckerTaskCallbackError{
        .code = TimerCheckerTaskCallbackErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }
  if (type != task && !adapted) {
    std::string message = ErrorTextTaskCallback::nothing(fn, type->name());

    return err(TimerCheckerTaskCallbackError{
        .code = TimerCheckerTaskCallbackErrorCode::CALLED_WITH_NOTHING,
        .message = message,
    });
  }

  return ok(type);
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

TimerChecker::ResultTimerScheduled TimerChecker::checkAsyncGlobal(CallExpr *call, NameExpr *name) {
  static const llvm::StringLiteral names[] = {"setTimeout",    "setInterval",   "setImmediate",
                                              "clearTimeout",  "clearInterval", "clearImmediate",
                                              "queueMicrotask"};
  auto isTimer = [&](llvm::StringRef n) {
    return this->checker.callChecker().isGlobalName(name, n);
  };
  if (!llvm::any_of(names, isTimer)) {
    return err(TimerCheckerCheckAsyncGlobalError{
        .code = TimerCheckerCheckAsyncGlobalErrorCode::NOT_A_TIMER,
        .message = "",
    });
  }
  this->checker.file_->usesAsync = true;
  call->type = Type::error();
  llvm::StringRef fn = name->name;
  if (this->checker.callChecker().rejectSpread(call)) {
    return err(TimerCheckerCheckAsyncGlobalError{
        .code = TimerCheckerCheckAsyncGlobalErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (fn == "setTimeout" || fn == "setInterval") {
    if (call->args.size() > 2) {
      this->checker.argumentChecker().checkArgsLoosely(call);
      std::string message = ErrorTextAsyncGlobal::callbackArg();

      return err(TimerCheckerCheckAsyncGlobalError{
          .code = TimerCheckerCheckAsyncGlobalErrorCode::TIMEOUT_CALLBACK_ARGUMENTS,
          .message = message,
      });
    }
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 2);
    if (arity.isErr()) {
      return err(TimerCheckerCheckAsyncGlobalError{
          .code = TimerCheckerCheckAsyncGlobalErrorCode::WRONG_ARITY,
          .message = arity.error().message,
      });
    }

    call->target =
        BuiltinCall{.builtin = fn == "setTimeout" ? Builtin::SetTimeout : Builtin::SetInterval};
    call->args[0] = this->checkTaskCallback(call->args[0], fn);
    if (call->args.size() > 1) {
      call->args[1] = this->checker.argumentChecker().checkAnyNumber(call->args[1]);
    }
    call->type = Type::timeout();
    if (call->args[0]->type->isError()) {
      return ok(call);
    }
    int bits = 0;
    if (fn == "setInterval") {
      if (!this->checker.emitterChecker().classifyListener(call->args[0], nullptr, bits, true)) {
        return ok(call);
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
    return ok(call);
  }
  if (fn == "setImmediate" || fn == "queueMicrotask") {
    if (call->args.size() > 1 && fn == "setImmediate") {
      this->checker.argumentChecker().checkArgsLoosely(call);
      std::string message = ErrorTextAsyncGlobal::callbackArg();

      return err(TimerCheckerCheckAsyncGlobalError{
          .code = TimerCheckerCheckAsyncGlobalErrorCode::IMMEDIATE_CALLBACK_ARGUMENTS,
          .message = message,
      });
    }
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 1);
    if (arity.isErr()) {
      return err(TimerCheckerCheckAsyncGlobalError{
          .code = TimerCheckerCheckAsyncGlobalErrorCode::WRONG_ARITY,
          .message = arity.error().message,
      });
    }

    call->target = BuiltinCall{.builtin = fn == "setImmediate" ? Builtin::SetImmediate
                                                               : Builtin::QueueMicrotask};
    call->args[0] = this->checkTaskCallback(call->args[0], fn);
    call->type = fn == "setImmediate" ? Type::immediate() : Type::voidTy();
    return ok(call);
  }
  ArgumentChecker::ResultArityMatches arity =
      this->checker.argumentChecker().checkArity(call, 0, 1);
  if (arity.isErr()) {
    return err(TimerCheckerCheckAsyncGlobalError{
        .code = TimerCheckerCheckAsyncGlobalErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
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

  return ok(call);
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

TimerChecker::ResultNextTickQueued TimerChecker::checkNextTick(CallExpr *call, MemberExpr *member) {
  this->checker.file_->usesAsync = true;
  call->type = Type::error();
  if (call->args.size() > 1) {
    this->checker.argumentChecker().checkArgsLoosely(call);
    std::string message = ErrorTextAsyncGlobal::callbackArg();

    return err(TimerCheckerCheckNextTickError{
        .code = TimerCheckerCheckNextTickErrorCode::CALLBACK_ARGUMENTS,
        .message = message,
    });
  }
  ArgumentChecker::ResultArityMatches arity =
      this->checker.argumentChecker().checkArity(call, 1, 1);
  if (arity.isErr()) {
    return err(TimerCheckerCheckNextTickError{
        .code = TimerCheckerCheckNextTickErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  call->target = BuiltinCall{.builtin = Builtin::NextTick};
  call->args[0] = this->checkTaskCallback(call->args[0], "process.nextTick");
  call->type = Type::voidTy();

  return ok(call);
}

TimerChecker::ResultTimerMethodRun TimerChecker::checkTimerMethod(CallExpr *call,
                                                                  MemberExpr *member) {
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
    this->checker.argumentChecker().checkArgsLoosely(call);
    std::string message = ErrorTextTimerMethod::noMethod(name, type->name(), type->isImmediate());

    return err(TimerCheckerCheckTimerMethodError{
        .code = TimerCheckerCheckTimerMethodErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }
  if (this->checker.callChecker().rejectSpread(call)) {
    return err(TimerCheckerCheckTimerMethodError{
        .code = TimerCheckerCheckTimerMethodErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  ArgumentChecker::ResultArityMatches arity =
      this->checker.argumentChecker().checkArity(call, 0, 0);
  if (arity.isErr()) {
    return err(TimerCheckerCheckTimerMethodError{
        .code = TimerCheckerCheckTimerMethodErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  call->target = BuiltinCall{.builtin = method->builtin};
  call->type = method->builtin == Builtin::TimerHasRef ? Type::boolTy() : type;

  return ok(call);
}

TimerChecker::ResultPromiseOfTimer TimerChecker::checkTimersPromisesCall(CallExpr *call,
                                                                         llvm::StringRef fn,
                                                                         llvm::SMLoc loc,
                                                                         const Type *expected) {
  this->checker.file_->usesAsync = true;
  call->type = Type::error();
  if (fn == "scheduler") {
    this->checker.argumentChecker().checkArgsLoosely(call);
    std::string message = ErrorTextTimersPromises::scheduler();

    return err(TimerCheckerCheckTimersPromisesCallError{
        .code = TimerCheckerCheckTimersPromisesCallErrorCode::SCHEDULER_ITSELF,
        .message = message,
    });
  }
  if (this->checker.callChecker().rejectSpread(call)) {
    return err(TimerCheckerCheckTimersPromisesCallError{
        .code = TimerCheckerCheckTimersPromisesCallErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  bool immediate = fn == "setImmediate";
  bool interval = fn == "setInterval";
  size_t valueAt = immediate ? 0 : 1;
  ArgumentChecker::ResultArityMatches arity =
      this->checker.argumentChecker().checkArity(call, 0, unsigned(valueAt + 2));
  if (arity.isErr()) {
    return err(TimerCheckerCheckTimersPromisesCallError{
        .code = TimerCheckerCheckTimersPromisesCallErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
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
      return err(TimerCheckerCheckTimersPromisesCallError{
          .code = TimerCheckerCheckTimersPromisesCallErrorCode::ALREADY_REPORTED,
          .message = "",
      });
    }
    if (value->isVoid()) {
      std::string message = ErrorTextTimersPromises::voidValue(fn);

      return err(TimerCheckerCheckTimersPromisesCallError{
          .code = TimerCheckerCheckTimersPromisesCallErrorCode::VOID_VALUE,
          .message = message,
      });
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
    return err(TimerCheckerCheckTimersPromisesCallError{
        .code = TimerCheckerCheckTimersPromisesCallErrorCode::ALREADY_REPORTED,
        .message = "",
    });
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
    return ok(call);
  }
  call->target =
      BuiltinCall{.builtin = immediate ? Builtin::TimersSetImmediate : Builtin::TimersSetTimeout};
  call->type = Type::promise(value);

  return ok(call);
}

TimerChecker::ResultSchedulerAwait TimerChecker::checkSchedulerCall(CallExpr *call,
                                                                    MemberExpr *member) {
  this->checker.file_->usesAsync = true;
  call->type = Type::error();
  bool known = member->name == "wait" || member->name == "yield";
  if (!known) {
    this->checker.argumentChecker().checkArgsLoosely(call);
    std::string message = ErrorTextScheduler::noFunction(member->name);

    return err(TimerCheckerCheckSchedulerCallError{
        .code = TimerCheckerCheckSchedulerCallErrorCode::NO_SUCH_FUNCTION,
        .message = message,
    });
  }
  if (this->checker.callChecker().rejectSpread(call)) {
    return err(TimerCheckerCheckSchedulerCallError{
        .code = TimerCheckerCheckSchedulerCallErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  bool wait = member->name == "wait";
  ArgumentChecker::ResultArityMatches arity =
      this->checker.argumentChecker().checkArity(call, wait ? 1 : 0, wait ? 2 : 0);
  if (arity.isErr()) {
    return err(TimerCheckerCheckSchedulerCallError{
        .code = TimerCheckerCheckSchedulerCallErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  if (wait) {
    Expr *options = call->args.size() > 1 ? call->args[1] : nullptr;
    auto *none = this->checker.ctx_.make<UndefinedExpr>(member->loc());
    call->args = {call->args[0], none};
    if (options) {
      call->args.push_back(options);
    }
  }

  ResultPromiseOfTimer promised = this->checkTimersPromisesCall(
      call, wait ? "setTimeout" : "setImmediate", member->loc(), nullptr);
  bool reported =
      promised.isErr() &&
      promised.error().code == TimerCheckerCheckTimersPromisesCallErrorCode::ALREADY_REPORTED;
  if (reported) {
    return err(TimerCheckerCheckSchedulerCallError{
        .code = TimerCheckerCheckSchedulerCallErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (promised.isErr()) {
    return err(TimerCheckerCheckSchedulerCallError{
        .code = TimerCheckerCheckSchedulerCallErrorCode::TIMER_FAILED,
        .message = promised.error().message,
    });
  }

  return ok(promised.value());
}

}
