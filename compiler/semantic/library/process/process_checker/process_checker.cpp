#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/library/async/timer_checker/timer_checker.h"
#include "compiler/semantic/library/date/date_checker/date_checker.h"
#include "compiler/semantic/library/events/event_target_checker/event_target_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker_errors.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/library/stream/stream_sources/stream_sources.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

ProcessChecker::ProcessChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.getCallChecker();
  this->dateChecker = &all.getDateChecker();
  this->exprChecker = &all.getExprChecker();
  this->timerChecker = &all.getTimerChecker();
  this->resultChecker = &all.getResultChecker();
  this->streamSources = &all.getStreamSources();
  this->typeRelations = &all.getTypeRelations();
  this->argumentChecker = &all.getArgumentChecker();
  this->eventTargetChecker = &all.getEventTargetChecker();
}

std::u16string ProcessChecker::toUtf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

bool ProcessChecker::isProcessMember(const Expr *expr, llvm::StringRef name) const {
  auto *member = dyn_cast<MemberExpr>(expr);

  return member && !member->optional && member->name == name &&
         this->callChecker->isGlobalName(member->object, GlobalName::PROCESS);
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
  code = this->exprChecker->checkExpr(code, Type::intTy());
  if (code->type->isOptional() || code->type->isUndefined()) {
    return this->typeRelations->coerce(code, optionalInt);
  }

  return this->typeRelations->convert(this->typeRelations->coerce(code, Type::intTy()),
                                      optionalInt);
}

Expr *ProcessChecker::checkProcessCall(CallExpr *call, MemberExpr *member) {
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
  };
  llvm::StringRef name = member->name;

  bool isDate = this->callChecker->isGlobalName(member->object, GlobalName::DATE);
  if (isDate && name != "now") {
    return this->dateChecker->checkDateStatic(call, member);
  }
  if (isDate || this->callChecker->isGlobalName(member->object, GlobalName::PERFORMANCE)) {
    llvm::StringRef object = isDate ? "Date" : "performance";
    if (name != "now") {
      std::string message = ErrorTextProcessCall::onlyNow(object, name);
      this->state.getDiagnostics().error(member->loc(), message);
      checkArgsLoosely();
      return call;
    }
    bool rejected = this->callChecker->rejectSpread(call);
    ArgumentChecker::ResultArityMatches arity =
        rejected ? ok(call) : this->argumentChecker->checkArity(call, 0, 0);
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
    bool rejected = this->callChecker->rejectSpread(call);
    ArgumentChecker::ResultArityMatches arity =
        rejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
    }
    if (rejected || arity.isErr()) {
      return call;
    }
    Expr *chunk = this->exprChecker->checkExpr(call->args[0], Type::string());
    const Type *type = chunk->type;
    if (!type->isBuffer() && type->bytes() != Type::Bytes::Uint8) {
      chunk = this->typeRelations->coerce(chunk, Type::string());
    }
    call->args[0] = chunk;
    call->target = BuiltinCall{.builtin = isStdout ? Builtin::ProcessStdoutWrite
                                                   : Builtin::ProcessStderrWrite};
    call->type = Type::boolTy();
    return call;
  }

  if (!this->callChecker->isGlobalName(member->object, GlobalName::PROCESS)) {
    return nullptr;
  }
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  if (name == "nextTick") {
    TimerChecker::ResultNextTickQueued queued = this->timerChecker->checkNextTick(call);
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
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
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
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->target = BuiltinCall{.builtin = Builtin::ProcessCwd};
    call->type = Type::string();
    return call;
  }
  if (name == "uptime") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->target = BuiltinCall{.builtin = Builtin::ProcessUptime};
    call->type = Type::doubleTy();
    return call;
  }
  // TODO: говнокод
  static const llvm::StringLiteral properties[] = {"argv",     "env",    "exitCode", "execPath",
                                                   "platform", "stdout", "stderr"};
  auto *event = call->args.empty() ? nullptr : dyn_cast<StringLitExpr>(call->args[0]);
  if (event && (event->value == u"uncaughtException" || event->value == u"unhandledRejection") &&
      (name == "on" || name == "once" || name == "addListener" || name == "prependListener")) {
    std::string message = ErrorTextProcessEvent::noThrown();
    this->state.getDiagnostics().error(call->args[0]->loc(), message);

    return this->resultChecker->markCallFailed(call);
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
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  auto *event = dyn_cast<StringLitExpr>(call->args[0]);
  ResultListenedSignal listened =
      event ? ProcessChecker::listenedSignal(name, event->value) : ok(name);
  if (listened.isErr()) {
    this->state.getDiagnostics().error(event->loc(), listened.error().message);
    call->args[1] = this->exprChecker->checkExpr(call->args[1]);
    return call;
  }
  Expr *listener = this->exprChecker->checkExpr(call->args[1]);
  call->args[1] = listener;
  if (listener->type->isError()) {
    return call;
  }
  bool waits = listener->type->isFunction() && listener->type->ret()->isPromise();
  // TODO: говнокод
  llvm::StringRef fn = name == "off" ? (waits ? "processOffAsync" : "processOff")
                                     : (waits ? "processOnAsync" : "processOn");
  if (name != "off") {
    auto *once = this->state.getAst().make<BoolLitExpr>(call->loc(), name == "once");
    once->type = Type::boolTy();
    call->args.push_back(once);
  }
  std::vector<bool> given(call->args.size(), true);
  given[0] = false;

  return this->eventTargetChecker->callLibraryFunction(call, fn, {}, given, member->loc());
}

// TODO: говнокод
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

  if (this->callChecker->isGlobalName(member->object, GlobalName::DATE)) {
    return this->dateChecker->checkDateStaticMember(member);
  }
  if (this->callChecker->isGlobalName(member->object, GlobalName::PERFORMANCE)) {
    std::string message =
        name == "now" ? ErrorTextProcessMember::perfNow() : ErrorTextProcessMember::perfOther(name);
    this->state.getDiagnostics().error(member->loc(), message);
    member->type = Type::error();
    return member;
  }

  if (!this->callChecker->isGlobalName(member->object, GlobalName::PROCESS)) {
    return nullptr;
  }
  if (name == "stdin" || name == "stdout" || name == "stderr") {
    if (Expr *stream = this->streamSources->processStream(member)) {
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
  Expr *key = this->argumentChecker->checkArg(index->index, Type::string());

  return this->builtinCall(index->loc(), index->object, Builtin::ProcessEnv, {key},
                           Type::optional(Type::string()));
}

// TODO: говнокод
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
  bool isProcess = this->callChecker->isGlobalName(object, GlobalName::PROCESS) && !name.empty();
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
  assign->value = this->exprChecker->checkExpr(assign->value);
  assign->type = Type::error();

  return assign;
}

}
