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
  this->callChecker = &all.callChecker;
  this->dateChecker = &all.dateChecker;
  this->exprChecker = &all.exprChecker;
  this->timerChecker = &all.timerChecker;
  this->resultChecker = &all.resultChecker;
  this->streamSources = &all.streamSources;
  this->typeRelations = &all.typeRelations;
  this->argumentChecker = &all.argumentChecker;
  this->eventTargetChecker = &all.eventTargetChecker;
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
  bool isDate = this->callChecker->isGlobalName(member->object, GlobalName::DATE);
  if (isDate && member->name != "now") {
    return this->dateChecker->checkDateStatic(call, member);
  }
  if (isDate || this->callChecker->isGlobalName(member->object, GlobalName::PERFORMANCE)) {
    return this->checkClockNow(call, member, isDate);
  }
  bool isStdout = this->isProcessMember(member->object, "stdout");
  if (isStdout || this->isProcessMember(member->object, "stderr")) {
    return this->checkStdStreamCall(call, member, isStdout);
  }
  if (!this->callChecker->isGlobalName(member->object, GlobalName::PROCESS)) {
    return nullptr;
  }
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  std::optional<ProcessMember> found = ProcessMemberTable::find(member->name);
  if (found == ProcessMember::NEXT_TICK) {
    return this->checkNextTick(call, member);
  }
  if (found == ProcessMember::EXIT) {
    return this->checkExit(call, member);
  }
  if (found == ProcessMember::CWD) {
    return this->checkNoArgCall(call, member, Builtin::ProcessCwd, Type::string());
  }
  if (found == ProcessMember::UPTIME) {
    return this->checkNoArgCall(call, member, Builtin::ProcessUptime, Type::doubleTy());
  }

  return this->checkProcessListenerCall(call, member, found);
}

void ProcessChecker::walkArgsLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }
}

bool ProcessChecker::hasRejectedArgs(CallExpr *call, MemberExpr *member, unsigned count) {
  bool isRejected = this->callChecker->rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      isRejected ? ok(call) : this->argumentChecker->checkArity(call, count, count);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }

  return isRejected || arity.isErr();
}

Expr *ProcessChecker::checkClockNow(CallExpr *call, MemberExpr *member, bool isDate) {
  if (member->name != "now") {
    llvm::StringRef object = isDate ? "Date" : "performance";
    std::string message = ErrorTextProcessCall::onlyNow(object, member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    this->walkArgsLoosely(call);
    return call;
  }
  if (this->hasRejectedArgs(call, member, 0)) {
    return call;
  }
  call->target = BuiltinCall{.builtin = isDate ? Builtin::DateNow : Builtin::PerformanceNow};
  call->type = isDate ? Type::intTy() : Type::doubleTy();

  return call;
}

Expr *ProcessChecker::checkStdStreamCall(CallExpr *call, MemberExpr *member, bool isStdout) {
  std::optional<StdStreamMember> found = StdStreamMemberTable::find(member->name);
  bool isTty = found == StdStreamMember::IS_TTY;
  if (!isTty && this->state.getLibraryDeclarations().findLibraryFunction("processStdout")) {
    return nullptr;
  }
  if (found != StdStreamMember::WRITE) {
    std::string stream = ErrorTextProcessStream::name(isStdout);
    std::string message = isTty ? ErrorTextProcessCall::ttyValue(stream)
                                : ErrorTextProcessCall::streamCall(stream, member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    this->walkArgsLoosely(call);
    return call;
  }
  if (this->hasRejectedArgs(call, member, 1)) {
    return call;
  }
  Expr *chunk = this->exprChecker->checkExpr(call->args[0], Type::string());
  const Type *type = chunk->type;
  if (!type->isBuffer() && type->bytes() != Type::Bytes::Uint8) {
    chunk = this->typeRelations->coerce(chunk, Type::string());
  }
  call->args[0] = chunk;
  Builtin builtin = isStdout ? Builtin::ProcessStdoutWrite : Builtin::ProcessStderrWrite;
  call->target = BuiltinCall{.builtin = builtin};
  call->type = Type::boolTy();

  return call;
}

Expr *ProcessChecker::checkNextTick(CallExpr *call, MemberExpr *member) {
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

Expr *ProcessChecker::checkExit(CallExpr *call, MemberExpr *member) {
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

Expr *ProcessChecker::checkNoArgCall(CallExpr *call, MemberExpr *member, Builtin builtin,
                                     const Type *result) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->target = BuiltinCall{.builtin = builtin};
  call->type = result;

  return call;
}

Expr *ProcessChecker::checkProcessListenerCall(CallExpr *call, MemberExpr *member,
                                               std::optional<ProcessMember> found) {
  bool isSubscribing = found == ProcessMember::ON || found == ProcessMember::ONCE ||
                       found == ProcessMember::ADD_LISTENER ||
                       found == ProcessMember::PREPEND_LISTENER;
  auto *event = call->args.empty() ? nullptr : dyn_cast<StringLitExpr>(call->args[0]);
  bool isThrown = event != nullptr &&
                  (event->value == u"uncaughtException" || event->value == u"unhandledRejection");
  if (isThrown && isSubscribing) {
    std::string message = ErrorTextProcessEvent::noThrown();
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    return this->resultChecker->markCallFailed(call);
  }
  bool isSignaling =
      found == ProcessMember::ON || found == ProcessMember::ONCE || found == ProcessMember::OFF;
  if (isSignaling && this->state.getLibraryDeclarations().findLibraryFunction("processOn")) {
    return this->checkSignalCall(call, member);
  }
  bool isProperty = found == ProcessMember::ARGV || found == ProcessMember::ENV ||
                    found == ProcessMember::EXIT_CODE || found == ProcessMember::EXEC_PATH ||
                    found == ProcessMember::PLATFORM || found == ProcessMember::STDOUT ||
                    found == ProcessMember::STDERR;
  std::string message = isProperty ? ErrorTextProcessCall::notFunc(member->name)
                                   : ErrorTextProcessCall::notYet(member->name);
  this->state.getDiagnostics().error(member->loc(), message);
  this->walkArgsLoosely(call);

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
  bool isWaiting = listener->type->isFunction() && listener->type->ret()->isPromise();
  // TODO: говнокод
  llvm::StringRef fn = name == "off" ? (isWaiting ? "processOffAsync" : "processOff")
                                     : (isWaiting ? "processOnAsync" : "processOn");
  if (name != "off") {
    auto *once = this->state.getAst().make<BoolLitExpr>(call->loc(), name == "once");
    once->type = Type::boolTy();
    call->args.push_back(once);
  }
  std::vector<bool> given(call->args.size(), true);
  given[0] = false;

  return this->eventTargetChecker->callLibraryFunction(call, fn, {}, given, member->loc());
}

Expr *ProcessChecker::checkProcessMember(MemberExpr *member) {
  if (this->isProcessMember(member->object, "env")) {
    std::u16string name = ProcessChecker::toUtf16(member->name);
    auto *key = this->state.getAst().make<StringLitExpr>(member->loc(), name);
    key->type = Type::string();
    return this->builtinCall(member->loc(), member, Builtin::ProcessEnv, {key},
                             Type::optional(Type::string()));
  }
  bool isStdout = this->isProcessMember(member->object, "stdout");
  if (isStdout || this->isProcessMember(member->object, "stderr")) {
    return this->checkStdStreamMember(member, isStdout);
  }
  if (this->callChecker->isGlobalName(member->object, GlobalName::DATE)) {
    return this->dateChecker->checkDateStaticMember(member);
  }
  if (this->callChecker->isGlobalName(member->object, GlobalName::PERFORMANCE)) {
    std::string message = member->name == "now" ? ErrorTextProcessMember::perfNow()
                                                : ErrorTextProcessMember::perfOther(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    member->type = Type::error();
    return member;
  }
  if (!this->callChecker->isGlobalName(member->object, GlobalName::PROCESS)) {
    return nullptr;
  }

  std::optional<ProcessMember> found = ProcessMemberTable::find(member->name);
  bool isStream = found == ProcessMember::STDIN || found == ProcessMember::STDOUT ||
                  found == ProcessMember::STDERR;
  if (isStream) {
    Expr *source = this->streamSources->processStream(member);
    if (source != nullptr) {
      return source;
    }
  }
  Expr *value = found.has_value() ? this->processValue(member, *found) : nullptr;
  if (value != nullptr) {
    return value;
  }
  member->type = Type::error();
  std::string message = ProcessChecker::processMemberMessage(member->name, found);
  this->state.getDiagnostics().error(member->loc(), message);

  return member;
}

Expr *ProcessChecker::checkStdStreamMember(MemberExpr *member, bool isStdout) {
  std::optional<StdStreamMember> found = StdStreamMemberTable::find(member->name);
  bool isKnown = found == StdStreamMember::IS_TTY || found == StdStreamMember::COLUMNS ||
                 found == StdStreamMember::ROWS;
  if (!isKnown && this->state.getLibraryDeclarations().findLibraryFunction("processStdout")) {
    return nullptr;
  }
  if (found == StdStreamMember::IS_TTY) {
    auto *fd = this->state.getAst().make<IntLitExpr>(member->loc(), isStdout ? 1 : 2);
    fd->type = Type::intTy();
    return this->builtinCall(member->loc(), member, Builtin::ProcessIsTTY, {fd}, Type::boolTy());
  }
  std::string stream = ErrorTextProcessStream::name(isStdout);
  std::string message = found == StdStreamMember::WRITE
                            ? ErrorTextProcessMember::writeCall(stream)
                            : ErrorTextProcessMember::streamPart(stream, member->name);
  this->state.getDiagnostics().error(member->loc(), message);
  member->type = Type::error();

  return member;
}

Expr *ProcessChecker::processValue(MemberExpr *member, ProcessMember found) {
  llvm::SMLoc loc = member->loc();
  switch (found) {
    case ProcessMember::ARGV:
      return this->builtinCall(loc, member, Builtin::ProcessArgv, {}, Type::array(Type::string()));
    case ProcessMember::EXIT_CODE:
      return this->builtinCall(loc, member, Builtin::ProcessExitCode, {},
                               Type::optional(Type::intTy()));
    case ProcessMember::EXEC_PATH:
      return this->builtinCall(loc, member, Builtin::ProcessExecPath, {}, Type::string());
    case ProcessMember::PLATFORM:
      return this->builtinCall(loc, member, Builtin::ProcessPlatform, {}, Type::string());
    case ProcessMember::PID:
      return this->builtinCall(loc, member, Builtin::ProcessPid, {}, Type::intTy());
    case ProcessMember::ENV:
      return this->builtinCall(loc, member, Builtin::EnvMap, {},
                               Type::dict(Type::DictKind::Record, Type::string(), Type::string()));
    case ProcessMember::NEXT_TICK:
    case ProcessMember::EXIT:
    case ProcessMember::CWD:
    case ProcessMember::UPTIME:
    case ProcessMember::ON:
    case ProcessMember::ONCE:
    case ProcessMember::OFF:
    case ProcessMember::ADD_LISTENER:
    case ProcessMember::PREPEND_LISTENER:
    case ProcessMember::STDIN:
    case ProcessMember::STDOUT:
    case ProcessMember::STDERR:
      return nullptr;
  }

  return nullptr;
}

std::string ProcessChecker::processMemberMessage(llvm::StringRef name,
                                                 std::optional<ProcessMember> found) {
  bool isFunction =
      found == ProcessMember::EXIT || found == ProcessMember::CWD || found == ProcessMember::UPTIME;
  if (isFunction) {
    return ErrorTextProcessMember::onlyCalled(name);
  }
  if (found == ProcessMember::STDOUT || found == ProcessMember::STDERR) {
    return ErrorTextProcessMember::streamUse(name);
  }

  return ErrorTextProcessCall::notYet(name);
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
