#include "compiler/semantic/library/async/timer_checker/timer_checker.h"
#include "compiler/program/library/async/scheduler_function_table/scheduler_function_table.h"
#include "compiler/program/library/async/timer_method_table/timer_method_table.h"
#include "compiler/program/library/async/timers_promises_function_table/timers_promises_function_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/process/process_qualified_name_table/process_qualified_name_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/bodies/functions/function_value_checker/function_value_checker.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
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
  this->fsChecker = &all.fsChecker;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->typeRelations = &all.typeRelations;
  this->closureChecker = &all.closureChecker;
  this->emitterChecker = &all.emitterChecker;
  this->argumentChecker = &all.argumentChecker;
  this->functionValueChecker = &all.functionValueChecker;
}

Expr *TimerChecker::checkTaskCallback(Expr *arg, llvm::StringRef functionName) {
  Signature signature;
  signature.ret = Type::voidTy();
  signature.voidResult = true;
  signature.asyncVoid = true;
  const auto *task = Type::function({}, 0, false, Type::voidTy());
  auto *functionExpr = dyn_cast<FunctionExpr>(arg);
  if (functionExpr != nullptr) {
    return this->closureChecker->checkFunctionExpr(functionExpr, &signature);
  }
  arg = this->exprChecker->checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  bool isAdaptable = type->isFunction() && type != task && type->minArgs() == 0 && !type->hasRest();
  Expr *adapted = isAdaptable ? this->functionValueChecker->adaptFunction(arg, task) : nullptr;
  auto fits = this->checkTaskCallbackType(type, task, adapted, functionName);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), fits.error().message);
    return arg;
  }

  return adapted != nullptr ? adapted : arg;
}

TimerChecker::ResultTaskCallbackOk
TimerChecker::checkTaskCallbackType(const Type *type, const Type *task, const Expr *adapted,
                                    llvm::StringRef functionName) const {
  if (!type->isFunction()) {
    auto message = ErrorTextTaskCallback::notFunction(functionName, type->name());

    return err(TimerCheckerTaskCallbackError{
        .code = TimerCheckerTaskCallbackErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }
  if (type != task && adapted == nullptr) {
    auto message = ErrorTextTaskCallback::nothing(functionName, type->name());

    return err(TimerCheckerTaskCallbackError{
        .code = TimerCheckerTaskCallbackErrorCode::CALLED_WITH_NOTHING,
        .message = message,
    });
  }

  return ok(type);
}

bool TimerChecker::isRearming(const Expr *callback) const {
  const auto *current = this->state.getFunctionStack().getCurrentDecl();
  if (current == nullptr || current->sym == nullptr) {
    return false;
  }
  const auto *bare = ExprParts::withoutImplicitCasts(callback);
  const auto *member = dyn_cast<MemberExpr>(bare);
  if (member != nullptr) {
    return member->method() == current->sym;
  }
  const auto *functionExpr = dyn_cast<FunctionExpr>(bare);
  if (functionExpr == nullptr || functionExpr->fn->body == nullptr ||
      functionExpr->fn->body->body.size() != 1) {
    return false;
  }
  const Stmt *only = functionExpr->fn->body->body.front();
  const auto *value = this->soleStatementValue(only);
  const auto *call = dyn_cast_or_null<CallExpr>(value);

  return call != nullptr && call->method() == current->sym;
}

const Expr *TimerChecker::soleStatementValue(const Stmt *statement) const {
  const auto *exprStmt = dyn_cast<ExprStmt>(statement);
  if (exprStmt != nullptr) {
    return exprStmt->expr;
  }
  const auto *returnStmt = dyn_cast<ReturnStmt>(statement);
  if (returnStmt != nullptr) {
    return returnStmt->value;
  }

  return nullptr;
}

TimerChecker::ResultTimerScheduled TimerChecker::checkAsyncGlobal(CallExpr *call, NameExpr *name) {
  if (!this->isTimerName(name)) {
    return err(TimerCheckerCheckAsyncGlobalError{
        .code = TimerCheckerCheckAsyncGlobalErrorCode::NOT_A_TIMER,
        .message = "",
    });
  }
  this->state.getInventoryStore().markUsesAsync();
  call->type = Type::error();
  auto global = GlobalNameTable::find(name->name);
  if (this->callChecker->rejectSpread(call)) {
    return err(TimerCheckerCheckAsyncGlobalError{
        .code = TimerCheckerCheckAsyncGlobalErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (global == GlobalName::SET_TIMEOUT || global == GlobalName::SET_INTERVAL) {
    return this->checkTimeoutCall(call, name->name, global == GlobalName::SET_INTERVAL);
  }
  if (global == GlobalName::SET_IMMEDIATE || global == GlobalName::QUEUE_MICROTASK) {
    return this->checkTaskQueueCall(call, name->name, global == GlobalName::SET_IMMEDIATE);
  }

  return this->checkClearCall(call, global == GlobalName::CLEAR_IMMEDIATE);
}

bool TimerChecker::isTimerName(const NameExpr *name) const {
  static const GlobalName names[] = {GlobalName::SET_TIMEOUT,    GlobalName::SET_INTERVAL,
                                     GlobalName::SET_IMMEDIATE,  GlobalName::CLEAR_TIMEOUT,
                                     GlobalName::CLEAR_INTERVAL, GlobalName::CLEAR_IMMEDIATE,
                                     GlobalName::QUEUE_MICROTASK};
  for (GlobalName timer : names) {
    if (this->callChecker->isGlobalName(name, timer)) {
      return true;
    }
  }

  return false;
}

TimerChecker::ResultTimerScheduled
TimerChecker::checkTimeoutCall(CallExpr *call, llvm::StringRef functionName, bool isInterval) {
  if (call->args.size() > 2) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextAsyncGlobal::callbackArg();

    return err(TimerCheckerCheckAsyncGlobalError{
        .code = TimerCheckerCheckAsyncGlobalErrorCode::TIMEOUT_CALLBACK_ARGUMENTS,
        .message = message,
    });
  }
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    return err(TimerCheckerCheckAsyncGlobalError{
        .code = TimerCheckerCheckAsyncGlobalErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }

  call->target = BuiltinCall{.builtin = isInterval ? Builtin::SetInterval : Builtin::SetTimeout};
  call->args[0] = this->checkTaskCallback(call->args[0], functionName);
  if (call->args.size() > 1) {
    call->args[1] = this->argumentChecker->checkAnyNumber(call->args[1]);
  }
  call->type = Type::timeout();
  if (call->args[0]->type->isError()) {
    return ok(call);
  }
  int bits = this->receiverBits(call->args[0], isInterval);
  if (bits != 0) {
    this->appendReceiverArgs(call, bits);
  }

  return ok(call);
}

int TimerChecker::receiverBits(Expr *callback, bool isInterval) {
  int bits = 0;
  if (isInterval) {
    bool isClassified =
        this->emitterChecker->classifyListener(callback, nullptr, bits, ListenerOwner::TIMER);
    return isClassified ? bits : 0;
  }
  if (this->isRearming(callback)) {
    this->emitterChecker->classifyListener(callback, nullptr, bits, ListenerOwner::TIMER);
    bits &= kTimerReceiver;
  }

  return bits;
}

void TimerChecker::appendReceiverArgs(CallExpr *call, int bits) {
  if (call->args.size() < 2) {
    auto *delay = this->state.getAst().make<IntLitExpr>(call->loc(), uint64_t(1));
    call->args.push_back(this->argumentChecker->checkAnyNumber(delay));
  }
  auto *receivers = this->state.getAst().make<IntLitExpr>(call->loc(), uint64_t(bits));
  call->args.push_back(this->exprChecker->checkExpr(receivers, Type::intTy()));
}

TimerChecker::ResultTimerScheduled
TimerChecker::checkTaskQueueCall(CallExpr *call, llvm::StringRef functionName, bool isImmediate) {
  if (call->args.size() > 1 && isImmediate) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextAsyncGlobal::callbackArg();

    return err(TimerCheckerCheckAsyncGlobalError{
        .code = TimerCheckerCheckAsyncGlobalErrorCode::IMMEDIATE_CALLBACK_ARGUMENTS,
        .message = message,
    });
  }
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    return err(TimerCheckerCheckAsyncGlobalError{
        .code = TimerCheckerCheckAsyncGlobalErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }

  call->target =
      BuiltinCall{.builtin = isImmediate ? Builtin::SetImmediate : Builtin::QueueMicrotask};
  call->args[0] = this->checkTaskCallback(call->args[0], functionName);
  call->type = isImmediate ? Type::immediate() : Type::voidTy();

  return ok(call);
}

TimerChecker::ResultTimerScheduled TimerChecker::checkClearCall(CallExpr *call, bool isImmediate) {
  auto arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    return err(TimerCheckerCheckAsyncGlobalError{
        .code = TimerCheckerCheckAsyncGlobalErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }

  call->target =
      BuiltinCall{.builtin = isImmediate ? Builtin::ClearImmediate : Builtin::ClearTimeout};
  if (call->args.empty()) {
    auto *none = this->state.getAst().make<UndefinedExpr>(call->loc());
    none->type = Type::undefinedTy();
    call->args.push_back(none);
  }
  const auto *handle = Type::optional(isImmediate ? Type::immediate() : Type::timeout());
  call->args[0] = this->argumentChecker->checkArg(call->args[0], handle);
  call->type = Type::voidTy();

  return ok(call);
}

const Type *TimerChecker::timerCallType(const Expr *init) const {
  auto *call = dyn_cast<CallExpr>(init);
  const Expr *callee = call != nullptr ? call->callee : nullptr;
  if (callee == nullptr) {
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
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextAsyncGlobal::callbackArg();

    return err(TimerCheckerCheckNextTickError{
        .code = TimerCheckerCheckNextTickErrorCode::CALLBACK_ARGUMENTS,
        .message = message,
    });
  }
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    return err(TimerCheckerCheckNextTickError{
        .code = TimerCheckerCheckNextTickErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  call->target = BuiltinCall{.builtin = Builtin::NextTick};
  auto nextTick = ProcessQualifiedNameTable::getQualifiedName(ProcessMember::NEXT_TICK);
  call->args[0] = this->checkTaskCallback(call->args[0], nextTick);
  call->type = Type::voidTy();

  return ok(call);
}

TimerChecker::ResultTimerMethodRun TimerChecker::checkTimerMethod(CallExpr *call,
                                                                  MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  call->type = Type::error();
  auto method = TimerMethodTable::find(name);
  bool isAvailable = method.has_value() && (method != TimerMethod::REFRESH || !type->isImmediate());
  if (!isAvailable) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextTimerMethod::noMethod(name, type->name(), type->isImmediate());

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
  auto arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    return err(TimerCheckerCheckTimerMethodError{
        .code = TimerCheckerCheckTimerMethodErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  Builtin builtin = this->timerMethodBuiltin(method.value());
  call->target = BuiltinCall{.builtin = builtin};
  call->type = method == TimerMethod::HAS_REF ? Type::boolTy() : type;

  return ok(call);
}

Builtin TimerChecker::timerMethodBuiltin(TimerMethod method) const {
  switch (method) {
    case TimerMethod::REF:
      return Builtin::TimerRef;
    case TimerMethod::UNREF:
      return Builtin::TimerUnref;
    case TimerMethod::HAS_REF:
      return Builtin::TimerHasRef;
    case TimerMethod::REFRESH:
      return Builtin::TimerRefresh;
  }

  return Builtin::TimerRef;
}

TimerChecker::ResultPromiseOfTimer
TimerChecker::checkTimersPromisesCall(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc,
                                      const Type *expected) {
  this->state.getInventoryStore().markUsesAsync();
  call->type = Type::error();
  auto function = TimersPromisesFunctionTable::find(functionName);
  if (function == TimersPromisesFunction::SCHEDULER) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextTimersPromises::scheduler();

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
  bool isImmediate = function == TimersPromisesFunction::SET_IMMEDIATE;
  bool isInterval = function == TimersPromisesFunction::SET_INTERVAL;
  size_t valueAt = isImmediate ? 0 : 1;
  auto arity = this->argumentChecker->checkArity(call, 0, unsigned(valueAt + 2));
  if (arity.isErr()) {
    return err(TimerCheckerCheckTimersPromisesCallError{
        .code = TimerCheckerCheckTimersPromisesCallErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }

  auto args = std::vector<Expr *>();
  if (!isImmediate) {
    auto *delay = this->checkDelayArg(call, loc);
    args.push_back(delay);
  }
  auto kept = this->checkTimerValue(call, valueAt, functionName, expected);
  if (kept.isErr()) {
    return err(TimerCheckerCheckTimersPromisesCallError{
        .code = kept.error().code,
        .message = kept.error().message,
    });
  }
  const Type *value = kept.value().value;
  if (kept.value().argument != nullptr) {
    args.push_back(kept.value().argument);
  }
  auto options = NodeOptions();
  bool isRead =
      this->fsChecker->readNodeOptions(call, valueAt + 1, functionName, {NodeOption::REF}, options);
  if (!isRead) {
    return err(TimerCheckerCheckTimersPromisesCallError{
        .code = TimerCheckerCheckTimersPromisesCallErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  auto *ref = this->checkRefOption(options.lookup(NodeOption::REF), loc);
  args.push_back(ref);
  call->args = std::move(args);
  call->spread.clear();

  return ok(this->promisedTimer(call, value, isImmediate, isInterval));
}

Expr *TimerChecker::checkDelayArg(CallExpr *call, llvm::SMLoc loc) {
  bool isMissing = call->args.empty() || isa<UndefinedExpr>(call->args[0]);
  if (!isMissing) {
    return this->argumentChecker->checkAnyNumber(call->args[0]);
  }
  auto *one = this->state.getAst().make<FloatLitExpr>(loc, 1.0);
  one->type = Type::doubleTy();

  return one;
}

TimerChecker::ResultTimerValueKept TimerChecker::checkTimerValue(CallExpr *call, size_t valueAt,
                                                                 llvm::StringRef functionName,
                                                                 const Type *expected) {
  TimerValue nothing{
      .value = Type::voidTy(),
      .argument = nullptr,
  };
  Expr *given = valueAt < call->args.size() ? call->args[valueAt] : nullptr;
  if (given == nullptr) {
    return ok(nothing);
  }
  if (isa<UndefinedExpr>(given)) {
    this->exprChecker->checkExpr(given);
    return ok(nothing);
  }
  const auto *wanted = this->promisedHint(expected);
  given = this->exprChecker->checkExpr(given, wanted);
  const Type *value = given->type;
  if (value->isError()) {
    return err(TimerCheckerCheckTimersPromisesCallError{
        .code = TimerCheckerCheckTimersPromisesCallErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (value->isVoid()) {
    auto message = ErrorTextTimersPromises::voidValue(functionName);

    return err(TimerCheckerCheckTimersPromisesCallError{
        .code = TimerCheckerCheckTimersPromisesCallErrorCode::VOID_VALUE,
        .message = message,
    });
  }
  if (value->isUndefined()) {
    return ok(nothing);
  }
  const Type *kept = value->isLiteral() ? value->literalBase() : value;

  return ok(TimerValue{
      .value = kept,
      .argument = this->typeRelations->coerce(given, kept),
  });
}

const Type *TimerChecker::promisedHint(const Type *expected) const {
  if (expected == nullptr || !expected->nonOptional()->isPromise()) {
    return nullptr;
  }
  const Type *wanted = expected->nonOptional()->promised();
  bool isEmpty = wanted->isVoid() || wanted->isNever();

  return isEmpty ? nullptr : wanted;
}

Expr *TimerChecker::checkRefOption(Expr *ref, llvm::SMLoc loc) {
  if (ref != nullptr) {
    return this->argumentChecker->checkArg(ref, Type::boolTy());
  }
  auto *yes = this->state.getAst().make<BoolLitExpr>(loc, true);
  yes->type = Type::boolTy();

  return yes;
}

Expr *TimerChecker::promisedTimer(CallExpr *call, const Type *value, bool isImmediate,
                                  bool isInterval) const {
  if (isInterval) {
    call->target = BuiltinCall{.builtin = Builtin::TimersSetInterval};
    call->type = Type::asyncIterator(value->isVoid() ? Type::undefinedTy() : value);
    return call;
  }
  call->target =
      BuiltinCall{.builtin = isImmediate ? Builtin::TimersSetImmediate : Builtin::TimersSetTimeout};
  call->type = Type::promise(value);

  return call;
}

TimerChecker::ResultSchedulerAwait TimerChecker::checkSchedulerCall(CallExpr *call,
                                                                    MemberExpr *member) {
  this->state.getInventoryStore().markUsesAsync();
  call->type = Type::error();
  auto function = SchedulerFunctionTable::find(member->name);
  if (!function.has_value()) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextScheduler::noFunction(member->name);

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
  bool isWait = function == SchedulerFunction::WAIT;
  unsigned minArgs = isWait ? 1 : 0;
  unsigned maxArgs = isWait ? 2 : 0;
  auto arity = this->argumentChecker->checkArity(call, minArgs, maxArgs);
  if (arity.isErr()) {
    return err(TimerCheckerCheckSchedulerCallError{
        .code = TimerCheckerCheckSchedulerCallErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  if (isWait) {
    Expr *options = call->args.size() > 1 ? call->args[1] : nullptr;
    auto *none = this->state.getAst().make<UndefinedExpr>(member->loc());
    call->args = {call->args[0], none};
    if (options != nullptr) {
      call->args.push_back(options);
    }
  }

  auto timer = isWait ? TimersPromisesFunction::SET_TIMEOUT : TimersPromisesFunction::SET_IMMEDIATE;
  auto timerName = TimersPromisesFunctionTable::getName(timer);
  auto promised = this->checkTimersPromisesCall(call, timerName, member->loc(), nullptr);
  bool isReported =
      promised.isErr() &&
      promised.error().code == TimerCheckerCheckTimersPromisesCallErrorCode::ALREADY_REPORTED;
  if (isReported) {
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
