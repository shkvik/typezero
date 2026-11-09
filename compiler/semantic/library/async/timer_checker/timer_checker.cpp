#include "compiler/semantic/library/async/timer_checker/timer_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/bodies/functions/function_value_checker/function_value_checker.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/fs/fs_checker/fs_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
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

TimerChecker::TimerChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fsChecker = &all.getFsChecker();
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
  this->typeRelations = &all.getTypeRelations();
  this->closureChecker = &all.getClosureChecker();
  this->emitterChecker = &all.getEmitterChecker();
  this->argumentChecker = &all.getArgumentChecker();
  this->functionValueChecker = &all.getFunctionValueChecker();
}

Expr *TimerChecker::checkTaskCallback(Expr *arg, llvm::StringRef fn) {
  Signature sig;
  sig.ret = Type::voidTy();
  sig.voidResult = true;
  sig.asyncVoid = true;
  const Type *task = Type::function({}, 0, false, Type::voidTy());
  if (isa<FunctionExpr>(arg)) {
    arg = this->closureChecker->checkFunctionExpr(cast<FunctionExpr>(arg), &sig);
    return arg;
  }
  arg = this->exprChecker->checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  bool adaptable = type->isFunction() && type != task && type->minArgs() == 0 && !type->hasRest();
  Expr *adapted = adaptable ? this->functionValueChecker->adaptFunction(arg, task) : nullptr;
  ResultTaskCallbackOk fits = TimerChecker::taskCallback(type, task, adapted, fn);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), fits.error().message);
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
  const FunctionDecl *current = this->state.getFunctionStack().getCurrentDecl();
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
  static const GlobalName names[] = {GlobalName::SET_TIMEOUT,    GlobalName::SET_INTERVAL,
                                     GlobalName::SET_IMMEDIATE,  GlobalName::CLEAR_TIMEOUT,
                                     GlobalName::CLEAR_INTERVAL, GlobalName::CLEAR_IMMEDIATE,
                                     GlobalName::QUEUE_MICROTASK};
  auto isTimer = [&](GlobalName timer) { return this->callChecker->isGlobalName(name, timer); };
  if (!llvm::any_of(names, isTimer)) {
    return err(TimerCheckerCheckAsyncGlobalError{
        .code = TimerCheckerCheckAsyncGlobalErrorCode::NOT_A_TIMER,
        .message = "",
    });
  }
  this->state.getInventoryStore().markUsesAsync();
  call->type = Type::error();
  llvm::StringRef fn = name->name;
  std::optional<GlobalName> global = GlobalNameTable::find(fn);
  if (this->callChecker->rejectSpread(call)) {
    return err(TimerCheckerCheckAsyncGlobalError{
        .code = TimerCheckerCheckAsyncGlobalErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (global == GlobalName::SET_TIMEOUT || global == GlobalName::SET_INTERVAL) {
    if (call->args.size() > 2) {
      this->argumentChecker->checkArgsLoosely(call);
      std::string message = ErrorTextAsyncGlobal::callbackArg();

      return err(TimerCheckerCheckAsyncGlobalError{
          .code = TimerCheckerCheckAsyncGlobalErrorCode::TIMEOUT_CALLBACK_ARGUMENTS,
          .message = message,
      });
    }
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
    if (arity.isErr()) {
      return err(TimerCheckerCheckAsyncGlobalError{
          .code = TimerCheckerCheckAsyncGlobalErrorCode::WRONG_ARITY,
          .message = arity.error().message,
      });
    }

    call->target = BuiltinCall{.builtin = global == GlobalName::SET_TIMEOUT ? Builtin::SetTimeout
                                                                            : Builtin::SetInterval};
    call->args[0] = this->checkTaskCallback(call->args[0], fn);
    if (call->args.size() > 1) {
      call->args[1] = this->argumentChecker->checkAnyNumber(call->args[1]);
    }
    call->type = Type::timeout();
    if (call->args[0]->type->isError()) {
      return ok(call);
    }
    int bits = 0;
    if (global == GlobalName::SET_INTERVAL) {
      if (!this->emitterChecker->classifyListener(call->args[0], nullptr, bits, true)) {
        return ok(call);
      }
    } else if (this->rearms(call->args[0])) {
      this->emitterChecker->classifyListener(call->args[0], nullptr, bits, true);
      bits &= kTimerReceiver;
    }
    if (bits) {
      if (call->args.size() < 2) {
        call->args.push_back(this->argumentChecker->checkAnyNumber(
            this->state.getAst().make<IntLitExpr>(call->loc(), uint64_t(1))));
      }
      call->args.push_back(this->exprChecker->checkExpr(
          this->state.getAst().make<IntLitExpr>(call->loc(), uint64_t(bits)), Type::intTy()));
    }
    return ok(call);
  }
  if (global == GlobalName::SET_IMMEDIATE || global == GlobalName::QUEUE_MICROTASK) {
    if (call->args.size() > 1 && global == GlobalName::SET_IMMEDIATE) {
      this->argumentChecker->checkArgsLoosely(call);
      std::string message = ErrorTextAsyncGlobal::callbackArg();

      return err(TimerCheckerCheckAsyncGlobalError{
          .code = TimerCheckerCheckAsyncGlobalErrorCode::IMMEDIATE_CALLBACK_ARGUMENTS,
          .message = message,
      });
    }
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      return err(TimerCheckerCheckAsyncGlobalError{
          .code = TimerCheckerCheckAsyncGlobalErrorCode::WRONG_ARITY,
          .message = arity.error().message,
      });
    }

    call->target =
        BuiltinCall{.builtin = global == GlobalName::SET_IMMEDIATE ? Builtin::SetImmediate
                                                                   : Builtin::QueueMicrotask};
    call->args[0] = this->checkTaskCallback(call->args[0], fn);
    call->type = global == GlobalName::SET_IMMEDIATE ? Type::immediate() : Type::voidTy();
    return ok(call);
  }
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    return err(TimerCheckerCheckAsyncGlobalError{
        .code = TimerCheckerCheckAsyncGlobalErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }

  bool immediate = global == GlobalName::CLEAR_IMMEDIATE;
  call->target =
      BuiltinCall{.builtin = immediate ? Builtin::ClearImmediate : Builtin::ClearTimeout};
  if (call->args.empty()) {
    auto *none = this->state.getAst().make<UndefinedExpr>(call->loc());
    none->type = Type::undefinedTy();
    call->args.push_back(none);
  }
  call->args[0] = this->argumentChecker->checkArg(
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
  if (this->callChecker->isGlobalName(callee, GlobalName::SET_TIMEOUT) ||
      this->callChecker->isGlobalName(callee, GlobalName::SET_INTERVAL)) {
    return Type::timeout();
  }
  if (this->callChecker->isGlobalName(callee, GlobalName::SET_IMMEDIATE)) {
    return Type::immediate();
  }

  return nullptr;
}

TimerChecker::ResultNextTickQueued TimerChecker::checkNextTick(CallExpr *call) {
  this->state.getInventoryStore().markUsesAsync();
  call->type = Type::error();
  if (call->args.size() > 1) {
    this->argumentChecker->checkArgsLoosely(call);
    std::string message = ErrorTextAsyncGlobal::callbackArg();

    return err(TimerCheckerCheckNextTickError{
        .code = TimerCheckerCheckNextTickErrorCode::CALLBACK_ARGUMENTS,
        .message = message,
    });
  }
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
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
    this->argumentChecker->checkArgsLoosely(call);
    std::string message = ErrorTextTimerMethod::noMethod(name, type->name(), type->isImmediate());

    return err(TimerCheckerCheckTimerMethodError{
        .code = TimerCheckerCheckTimerMethodErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }
  if (this->callChecker->rejectSpread(call)) {
    return err(TimerCheckerCheckTimerMethodError{
        .code = TimerCheckerCheckTimerMethodErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
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
  this->state.getInventoryStore().markUsesAsync();
  call->type = Type::error();
  if (fn == "scheduler") {
    this->argumentChecker->checkArgsLoosely(call);
    std::string message = ErrorTextTimersPromises::scheduler();

    return err(TimerCheckerCheckTimersPromisesCallError{
        .code = TimerCheckerCheckTimersPromisesCallErrorCode::SCHEDULER_ITSELF,
        .message = message,
    });
  }
  if (this->callChecker->rejectSpread(call)) {
    return err(TimerCheckerCheckTimersPromisesCallError{
        .code = TimerCheckerCheckTimersPromisesCallErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  bool immediate = fn == "setImmediate";
  bool interval = fn == "setInterval";
  size_t valueAt = immediate ? 0 : 1;
  ArgumentChecker::ResultArityMatches arity =
      this->argumentChecker->checkArity(call, 0, unsigned(valueAt + 2));
  if (arity.isErr()) {
    return err(TimerCheckerCheckTimersPromisesCallError{
        .code = TimerCheckerCheckTimersPromisesCallErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }

  std::vector<Expr *> args;
  if (!immediate) {
    if (call->args.empty() || isa<UndefinedExpr>(call->args[0])) {
      auto *one = this->state.getAst().make<FloatLitExpr>(loc, 1.0);
      one->type = Type::doubleTy();
      args.push_back(one);
    } else {
      args.push_back(this->argumentChecker->checkAnyNumber(call->args[0]));
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
    given = this->exprChecker->checkExpr(given, wanted);
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
      given = this->typeRelations->coerce(given, value);
      args.push_back(given);
    }
  } else if (given) {
    this->exprChecker->checkExpr(given);
  }
  llvm::StringMap<Expr *> options;
  if (!this->fsChecker->nodeOptions(call, valueAt + 1, fn, {"ref"}, options)) {
    return err(TimerCheckerCheckTimersPromisesCallError{
        .code = TimerCheckerCheckTimersPromisesCallErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (Expr *ref = options.lookup("ref")) {
    args.push_back(this->argumentChecker->checkArg(ref, Type::boolTy()));
  } else {
    auto *yes = this->state.getAst().make<BoolLitExpr>(loc, true);
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
  this->state.getInventoryStore().markUsesAsync();
  call->type = Type::error();
  bool known = member->name == "wait" || member->name == "yield";
  if (!known) {
    this->argumentChecker->checkArgsLoosely(call);
    std::string message = ErrorTextScheduler::noFunction(member->name);

    return err(TimerCheckerCheckSchedulerCallError{
        .code = TimerCheckerCheckSchedulerCallErrorCode::NO_SUCH_FUNCTION,
        .message = message,
    });
  }
  if (this->callChecker->rejectSpread(call)) {
    return err(TimerCheckerCheckSchedulerCallError{
        .code = TimerCheckerCheckSchedulerCallErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  bool wait = member->name == "wait";
  ArgumentChecker::ResultArityMatches arity =
      this->argumentChecker->checkArity(call, wait ? 1 : 0, wait ? 2 : 0);
  if (arity.isErr()) {
    return err(TimerCheckerCheckSchedulerCallError{
        .code = TimerCheckerCheckSchedulerCallErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  if (wait) {
    Expr *options = call->args.size() > 1 ? call->args[1] : nullptr;
    auto *none = this->state.getAst().make<UndefinedExpr>(member->loc());
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
