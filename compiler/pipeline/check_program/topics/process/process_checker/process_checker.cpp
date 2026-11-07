#include "compiler/pipeline/check_program/topics/process/process_checker/process_checker.h"
#include "compiler/load/to_utf8/to_utf8.h"
#include "compiler/pipeline/check_program/topics/async/timer_checker/timer_checker.h"
#include "compiler/pipeline/check_program/topics/date/date_checker/date_checker.h"
#include "compiler/pipeline/check_program/topics/events/event_target_checker/event_target_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/process/process_checker/process_checker_errors.h"
#include "compiler/pipeline/check_program/topics/result/result_checker/result_checker.h"
#include "compiler/pipeline/check_program/topics/stream/stream_sources/stream_sources.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

std::u16string ProcessChecker::toUtf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

ProcessChecker::ProcessChecker(CheckState &owner)
    : state(owner) {}

bool ProcessChecker::isProcessMember(const Expr *expr, llvm::StringRef name) const {
  auto *member = dyn_cast<MemberExpr>(expr);

  return member && !member->optional && member->name == name &&
         CallChecker(this->state).isGlobalName(member->object, "process");
}

Expr *ProcessChecker::builtinCall(llvm::SMLoc loc, Expr *callee, Builtin builtin,
                                  std::vector<Expr *> args, const Type *type) {
  auto *call = this->state.getAst().make<CallExpr>(loc, callee, std::move(args));
  call->target = BuiltinCall{.builtin = builtin};
  call->type = type;

  return call;
}

Expr *ProcessChecker::checkExitCode(Expr *code) {
  const Type *optionalInt = Type::optional(Type::intTy());
  code = ExprChecker(this->state).checkExpr(code, Type::intTy());
  if (code->type->isOptional() || code->type->isUndefined()) {
    return TypeRelations(this->state).coerce(code, optionalInt);
  }

  return TypeRelations(this->state)
      .convert(TypeRelations(this->state).coerce(code, Type::intTy()), optionalInt);
}

Expr *ProcessChecker::checkProcessCall(CallExpr *call, MemberExpr *member) {
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
  };
  llvm::StringRef name = member->name;

  bool isDate = CallChecker(this->state).isGlobalName(member->object, "Date");
  if (isDate && name != "now") {
    return DateChecker(this->state).checkDateStatic(call, member);
  }
  if (isDate || CallChecker(this->state).isGlobalName(member->object, "performance")) {
    llvm::StringRef object = isDate ? "Date" : "performance";
    if (name != "now") {
      std::string message = ErrorTextProcessCall::onlyNow(object, name);
      this->state.getDiagnostics().error(member->loc(), message);
      checkArgsLoosely();
      return call;
    }
    bool rejected = CallChecker(this->state).rejectSpread(call);
    ArgumentChecker::ResultArityMatches arity =
        rejected ? ok(call) : ArgumentChecker(this->state).checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
    }
    if (rejected || arity.isErr()) {
      return call;
    }
    call->target = BuiltinCall{.builtin = isDate ? Builtin::DateNow : Builtin::PerformanceNow};
    call->type = isDate ? Type::intTy() : Type::doubleTy();
    return call;
  }

  bool isStdout = this->isProcessMember(member->object, "stdout");
  if (isStdout || this->isProcessMember(member->object, "stderr")) {
    std::string stream = ErrorTextProcessStream::name(isStdout);
    if (name != "isTTY" &&
        this->state.getLibraryDeclarations().findLibraryFunction("processStdout")) {
      return nullptr;
    }
    if (name != "write") {
      std::string message = name == "isTTY" ? ErrorTextProcessCall::ttyValue(stream)
                                            : ErrorTextProcessCall::streamCall(stream, name);
      this->state.getDiagnostics().error(member->loc(), message);
      checkArgsLoosely();
      return call;
    }
    bool rejected = CallChecker(this->state).rejectSpread(call);
    ArgumentChecker::ResultArityMatches arity =
        rejected ? ok(call) : ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
    }
    if (rejected || arity.isErr()) {
      return call;
    }
    Expr *chunk = ExprChecker(this->state).checkExpr(call->args[0], Type::string());
    const Type *type = chunk->type;
    if (!type->isBuffer() && type->bytes() != Type::Bytes::Uint8) {
      chunk = TypeRelations(this->state).coerce(chunk, Type::string());
    }
    call->args[0] = chunk;
    call->target = BuiltinCall{.builtin = isStdout ? Builtin::ProcessStdoutWrite
                                                   : Builtin::ProcessStderrWrite};
    call->type = Type::boolTy();
    return call;
  }

  if (!CallChecker(this->state).isGlobalName(member->object, "process")) {
    return nullptr;
  }
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  if (name == "nextTick") {
    TimerChecker::ResultNextTickQueued queued = TimerChecker(this->state).checkNextTick(call);
    if (queued.isOk()) {
      return queued.value();
    }
    llvm::SMLoc at = member->loc();
    switch (queued.error().code) {
      case TimerCheckerCheckNextTickErrorCode::CALLBACK_ARGUMENTS:
        at = call->args[1]->loc();
        break;
      case TimerCheckerCheckNextTickErrorCode::WRONG_ARITY:
        break;
    }
    this->state.getDiagnostics().error(at, queued.error().message);
    return call;
  }
  if (name == "exit") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    if (!call->args.empty()) {
      call->args[0] = this->checkExitCode(call->args[0]);
    }
    call->target = BuiltinCall{.builtin = Builtin::ProcessExit};
    call->type = Type::voidTy();
    return call;
  }
  if (name == "cwd") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->target = BuiltinCall{.builtin = Builtin::ProcessCwd};
    call->type = Type::string();
    return call;
  }
  if (name == "uptime") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->target = BuiltinCall{.builtin = Builtin::ProcessUptime};
    call->type = Type::doubleTy();
    return call;
  }
  static const llvm::StringLiteral properties[] = {"argv",     "env",    "exitCode", "execPath",
                                                   "platform", "stdout", "stderr"};
  auto *event = call->args.empty() ? nullptr : dyn_cast<StringLitExpr>(call->args[0]);
  if (event && (event->value == u"uncaughtException" || event->value == u"unhandledRejection") &&
      (name == "on" || name == "once" || name == "addListener" || name == "prependListener")) {
    std::string message = ErrorTextProcessEvent::noThrown();
    this->state.getDiagnostics().error(call->args[0]->loc(), message);

    return ResultChecker(this->state).markCallFailed(call);
  }
  if ((name == "on" || name == "once" || name == "off") &&
      this->state.getLibraryDeclarations().findLibraryFunction("processOn")) {
    return this->checkSignalCall(call, member);
  }
  std::string message = llvm::is_contained(properties, name) ? ErrorTextProcessCall::notFunc(name)
                                                             : ErrorTextProcessCall::notYet(name);
  this->state.getDiagnostics().error(member->loc(), message);
  checkArgsLoosely();

  return call;
}

ProcessChecker::ResultListenedSignal ProcessChecker::listenedSignal(llvm::StringRef name,
                                                                    const std::u16string &event) {
  static const char16_t *const signals[] = {u"SIGINT", u"SIGTERM", u"SIGHUP", u"SIGBREAK"};
  if (llvm::none_of(signals, [&](const char16_t *s) { return event == s; })) {
    std::string written = ToUtf8::toUtf8(event);
    std::string message = ErrorTextProcessCall::signal(name, written);

    return err(ProcessCheckerListenedSignalError{
        .code = ProcessCheckerListenedSignalErrorCode::NOT_A_SIGNAL,
        .message = message,
    });
  }

  return ok(name);
}

Expr *ProcessChecker::checkSignalCall(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  call->type = Type::error();
  ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  auto *event = dyn_cast<StringLitExpr>(call->args[0]);
  ResultListenedSignal listened =
      event ? ProcessChecker::listenedSignal(name, event->value) : ok(name);
  if (listened.isErr()) {
    this->state.getDiagnostics().error(event->loc(), listened.error().message);
    call->args[1] = ExprChecker(this->state).checkExpr(call->args[1]);
    return call;
  }
  Expr *listener = ExprChecker(this->state).checkExpr(call->args[1]);
  call->args[1] = listener;
  if (listener->type->isError()) {
    return call;
  }
  bool waits = listener->type->isFunction() && listener->type->ret()->isPromise();
  llvm::StringRef fn = name == "off" ? (waits ? "processOffAsync" : "processOff")
                                     : (waits ? "processOnAsync" : "processOn");
  if (name != "off") {
    auto *once = this->state.getAst().make<BoolLitExpr>(call->loc(), name == "once");
    once->type = Type::boolTy();
    call->args.push_back(once);
  }
  std::vector<bool> given(call->args.size(), true);
  given[0] = false;

  return EventTargetChecker(this->state).callLibraryFunction(call, fn, {}, given, member->loc());
}

Expr *ProcessChecker::checkProcessMember(MemberExpr *member) {
  llvm::StringRef name = member->name;
  if (this->isProcessMember(member->object, "env")) {
    auto *key =
        this->state.getAst().make<StringLitExpr>(member->loc(), ProcessChecker::toUtf16(name));
    key->type = Type::string();
    return this->builtinCall(member->loc(), member, Builtin::ProcessEnv, {key},
                             Type::optional(Type::string()));
  }

  bool isStdout = this->isProcessMember(member->object, "stdout");
  if (isStdout || this->isProcessMember(member->object, "stderr")) {
    std::string stream = ErrorTextProcessStream::name(isStdout);
    if (name != "isTTY" && name != "columns" && name != "rows" &&
        this->state.getLibraryDeclarations().findLibraryFunction("processStdout")) {
      return nullptr;
    }
    if (name == "isTTY") {
      auto *fd = this->state.getAst().make<IntLitExpr>(member->loc(), isStdout ? 1 : 2);
      fd->type = Type::intTy();
      return this->builtinCall(member->loc(), member, Builtin::ProcessIsTTY, {fd}, Type::boolTy());
    }
    std::string message = name == "write" ? ErrorTextProcessMember::writeCall(stream)
                                          : ErrorTextProcessMember::streamPart(stream, name);
    this->state.getDiagnostics().error(member->loc(), message);
    member->type = Type::error();
    return member;
  }

  if (CallChecker(this->state).isGlobalName(member->object, "Date")) {
    return DateChecker(this->state).checkDateStaticMember(member);
  }
  if (CallChecker(this->state).isGlobalName(member->object, "performance")) {
    std::string message =
        name == "now" ? ErrorTextProcessMember::perfNow() : ErrorTextProcessMember::perfOther(name);
    this->state.getDiagnostics().error(member->loc(), message);
    member->type = Type::error();
    return member;
  }

  if (!CallChecker(this->state).isGlobalName(member->object, "process")) {
    return nullptr;
  }
  if (name == "stdin" || name == "stdout" || name == "stderr") {
    if (Expr *stream = StreamSources(this->state).processStream(member)) {
      return stream;
    }
  }
  if (name == "argv") {
    return this->builtinCall(member->loc(), member, Builtin::ProcessArgv, {},
                             Type::array(Type::string()));
  }
  if (name == "exitCode") {
    return this->builtinCall(member->loc(), member, Builtin::ProcessExitCode, {},
                             Type::optional(Type::intTy()));
  }
  if (name == "execPath") {
    return this->builtinCall(member->loc(), member, Builtin::ProcessExecPath, {}, Type::string());
  }
  if (name == "platform") {
    return this->builtinCall(member->loc(), member, Builtin::ProcessPlatform, {}, Type::string());
  }
  if (name == "pid") {
    return this->builtinCall(member->loc(), member, Builtin::ProcessPid, {}, Type::intTy());
  }
  if (name == "env") {
    return this->builtinCall(member->loc(), member, Builtin::EnvMap, {},
                             Type::dict(Type::DictKind::Record, Type::string(), Type::string()));
  }
  member->type = Type::error();
  std::string message =
      name == "exit" || name == "cwd" || name == "uptime" ? ErrorTextProcessMember::onlyCalled(name)
      : name == "stdout" || name == "stderr"              ? ErrorTextProcessMember::streamUse(name)
                                                          : ErrorTextProcessCall::notYet(name);
  this->state.getDiagnostics().error(member->loc(), message);

  return member;
}

Expr *ProcessChecker::checkProcessEnvIndex(IndexExpr *index) {
  if (!this->isProcessMember(index->object, "env")) {
    return nullptr;
  }
  Expr *key = ArgumentChecker(this->state).checkArg(index->index, Type::string());

  return this->builtinCall(index->loc(), index->object, Builtin::ProcessEnv, {key},
                           Type::optional(Type::string()));
}

Expr *ProcessChecker::checkProcessAssign(AssignExpr *assign) {
  Expr *object = nullptr;
  llvm::StringRef name;
  if (auto *member = dyn_cast<MemberExpr>(assign->target)) {
    object = member->object;
    name = member->name;
  } else if (auto *index = dyn_cast<IndexExpr>(assign->target)) {
    object = index->object;
  }
  if (!object) {
    return nullptr;
  }
  bool isEnv = this->isProcessMember(object, "env");
  bool isProcess = CallChecker(this->state).isGlobalName(object, "process") && !name.empty();
  bool isStream =
      this->isProcessMember(object, "stdout") || this->isProcessMember(object, "stderr");
  if (!isEnv && !isProcess && !isStream) {
    return nullptr;
  }

  if (isProcess && name == "exitCode" && !assign->op) {
    Expr *code = this->checkExitCode(assign->value);
    return this->builtinCall(assign->loc(), assign->target, Builtin::ProcessSetExitCode, {code},
                             Type::voidTy());
  }
  if (isEnv) {
    return nullptr;
  }
  if (isProcess && name == "exitCode") {
    std::string message = ErrorTextProcessAssign::exitCode();
    this->state.getDiagnostics().error(assign->loc(), message);
  } else {
    std::string message = isProcess
                              ? ErrorTextProcessAssign::assigned(name)
                              : ErrorTextProcessAssign::changed(cast<MemberExpr>(object)->name);
    this->state.getDiagnostics().error(assign->target->loc(), message);
  }
  assign->value = ExprChecker(this->state).checkExpr(assign->value);
  assign->type = Type::error();

  return assign;
}

}
