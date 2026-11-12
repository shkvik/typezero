#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/process/process_event_table/process_event_table.h"
#include "compiler/program/library/process/process_function_table/process_function_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
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

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

namespace {

constexpr uint64_t kStdoutDescriptor = 1;
constexpr uint64_t kStderrDescriptor = 2;

}

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

bool ProcessChecker::isProcessMember(const Expr *expr, ProcessMember which) const {
  auto *member = dyn_cast<MemberExpr>(expr);
  if (member == nullptr || member->optional) {
    return false;
  }
  if (member->name != ProcessMemberTable::getName(which)) {
    return false;
  }

  return this->callChecker->isGlobalName(member->object, GlobalName::PROCESS);
}

std::optional<ProcessMember> ProcessChecker::outputStream(const Expr *expr) const {
  if (this->isProcessMember(expr, ProcessMember::STDOUT)) {
    return ProcessMember::STDOUT;
  }
  if (this->isProcessMember(expr, ProcessMember::STDERR)) {
    return ProcessMember::STDERR;
  }

  return std::nullopt;
}

bool ProcessChecker::hasStreamLibrary() const {
  llvm::StringRef name = ProcessFunctionTable::getName(ProcessFunction::STDOUT);

  return this->state.getLibraryDeclarations().findLibraryFunction(name) != nullptr;
}

bool ProcessChecker::hasSignalLibrary() const {
  llvm::StringRef name = ProcessFunctionTable::getName(ProcessFunction::ON);

  return this->state.getLibraryDeclarations().findLibraryFunction(name) != nullptr;
}

Expr *ProcessChecker::builtinCall(llvm::SMLoc loc, Expr *callee, Builtin builtin,
                                  std::vector<Expr *> args, const Type *type) {
  auto *call = this->state.getAst().make<CallExpr>(loc, callee, std::move(args));
  call->target = BuiltinCall{.builtin = builtin};
  call->type = type;

  return call;
}

Expr *ProcessChecker::checkExitCode(Expr *code) {
  const auto *optionalInt = Type::optional(Type::intTy());
  auto *checked = this->exprChecker->checkExpr(code, Type::intTy());
  if (checked->type->isOptional() || checked->type->isUndefined()) {
    return this->typeRelations->coerce(checked, optionalInt);
  }
  auto *whole = this->typeRelations->coerce(checked, Type::intTy());

  return this->typeRelations->convert(whole, optionalInt);
}

Expr *ProcessChecker::checkProcessCall(CallExpr *call, MemberExpr *member) {
  if (this->callChecker->isGlobalMember(member, GlobalMember::DATE_NOW)) {
    return this->checkClockNow(call, member, Builtin::DateNow, Type::intTy());
  }
  if (this->callChecker->isGlobalName(member->object, GlobalName::DATE)) {
    return this->dateChecker->checkDateStatic(call, member);
  }
  if (this->callChecker->isGlobalMember(member, GlobalMember::PERFORMANCE_NOW)) {
    return this->checkClockNow(call, member, Builtin::PerformanceNow, Type::doubleTy());
  }
  if (this->callChecker->isGlobalName(member->object, GlobalName::PERFORMANCE)) {
    return this->rejectPerformanceCall(call, member);
  }
  auto stream = this->outputStream(member->object);
  if (stream.has_value()) {
    return this->checkStdStreamCall(call, member, *stream);
  }
  if (!this->callChecker->isGlobalName(member->object, GlobalName::PROCESS)) {
    return nullptr;
  }
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  auto found = ProcessMemberTable::find(member->name);
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

bool ProcessChecker::hasRejectedArgs(CallExpr *call, const MemberExpr *member, unsigned count) {
  if (this->callChecker->rejectSpread(call)) {
    return true;
  }
  auto arity = this->argumentChecker->checkArity(call, count, count);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }

  return arity.isErr();
}

Expr *ProcessChecker::checkClockNow(CallExpr *call, const MemberExpr *member, Builtin builtin,
                                    const Type *result) {
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }

  return this->checkNoArgCall(call, member, builtin, result);
}

Expr *ProcessChecker::rejectPerformanceCall(CallExpr *call, const MemberExpr *member) {
  llvm::StringRef object = GlobalNameTable::getName(GlobalName::PERFORMANCE);
  auto message = ErrorTextProcessCall::onlyNow(object, member->name);
  this->state.getDiagnostics().error(member->loc(), message);
  this->walkArgsLoosely(call);

  return call;
}

Expr *ProcessChecker::checkStdStreamCall(CallExpr *call, const MemberExpr *member,
                                         ProcessMember stream) {
  bool isStdout = stream == ProcessMember::STDOUT;
  auto found = StdStreamMemberTable::find(member->name);
  bool isTty = found == StdStreamMember::IS_TTY;
  if (!isTty && this->hasStreamLibrary()) {
    return nullptr;
  }
  if (found != StdStreamMember::WRITE) {
    auto shown = ErrorTextProcessStream::name(isStdout);
    std::string message = isTty ? ErrorTextProcessCall::ttyValue(shown)
                                : ErrorTextProcessCall::streamCall(shown, member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    this->walkArgsLoosely(call);

    return call;
  }
  if (this->hasRejectedArgs(call, member, 1)) {
    return call;
  }
  auto *checked = this->exprChecker->checkExpr(call->args[0], Type::string());
  call->args[0] = this->writtenChunk(checked);
  Builtin builtin = isStdout ? Builtin::ProcessStdoutWrite : Builtin::ProcessStderrWrite;
  call->target = BuiltinCall{.builtin = builtin};
  call->type = Type::boolTy();

  return call;
}

Expr *ProcessChecker::writtenChunk(Expr *chunk) {
  const Type *type = chunk->type;
  if (type->isBuffer() || type->bytes() == Type::Bytes::Uint8) {
    return chunk;
  }

  return this->typeRelations->coerce(chunk, Type::string());
}

Expr *ProcessChecker::checkNextTick(CallExpr *call, const MemberExpr *member) {
  auto queued = this->timerChecker->checkNextTick(call);
  if (queued.isOk()) {
    return queued.value();
  }
  switch (queued.error().code) {
    case TimerCheckerCheckNextTickErrorCode::CALLBACK_ARGUMENTS:
      this->state.getDiagnostics().error(call->args[1]->loc(), queued.error().message);
      break;
    case TimerCheckerCheckNextTickErrorCode::WRONG_ARITY:
      this->state.getDiagnostics().error(member->loc(), queued.error().message);
      break;
  }

  return call;
}

Expr *ProcessChecker::checkExit(CallExpr *call, const MemberExpr *member) {
  auto arity = this->argumentChecker->checkArity(call, 0, 1);
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

Expr *ProcessChecker::checkNoArgCall(CallExpr *call, const MemberExpr *member, Builtin builtin,
                                     const Type *result) {
  auto arity = this->argumentChecker->checkArity(call, 0, 0);
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
  if (this->isSubscribing(found) && this->isThrownEvent(call)) {
    auto message = ErrorTextProcessEvent::noThrown();
    this->state.getDiagnostics().error(call->args[0]->loc(), message);

    return this->resultChecker->markCallFailed(call);
  }
  if (this->isSignalMethod(found) && this->hasSignalLibrary()) {
    return this->checkSignalCall(call, member, *found);
  }
  std::string message = this->isProperty(found) ? ErrorTextProcessCall::notFunc(member->name)
                                                : ErrorTextProcessCall::notYet(member->name);
  this->state.getDiagnostics().error(member->loc(), message);
  this->walkArgsLoosely(call);

  return call;
}

bool ProcessChecker::isSubscribing(std::optional<ProcessMember> found) const {
  return found == ProcessMember::ON || found == ProcessMember::ONCE ||
         found == ProcessMember::ADD_LISTENER || found == ProcessMember::PREPEND_LISTENER;
}

bool ProcessChecker::isSignalMethod(std::optional<ProcessMember> found) const {
  return found == ProcessMember::ON || found == ProcessMember::ONCE || found == ProcessMember::OFF;
}

bool ProcessChecker::isProperty(std::optional<ProcessMember> found) const {
  bool isValue = found == ProcessMember::ARGV || found == ProcessMember::ENV ||
                 found == ProcessMember::EXIT_CODE || found == ProcessMember::EXEC_PATH ||
                 found == ProcessMember::PLATFORM;

  return isValue || found == ProcessMember::STDOUT || found == ProcessMember::STDERR;
}

bool ProcessChecker::isThrownEvent(const CallExpr *call) const {
  if (call->args.empty()) {
    return false;
  }
  auto *event = dyn_cast<StringLitExpr>(call->args[0]);
  if (event == nullptr) {
    return false;
  }
  auto written = ToUtf8::toUtf8(event->value);
  const auto *known = ProcessEventTable::find(written);

  return known != nullptr && known->kind == ProcessEventKind::THROWN;
}

ProcessChecker::ResultListenedSignal ProcessChecker::checkListenedSignal(llvm::StringRef name,
                                                                         const Expr *event) const {
  auto *literal = dyn_cast<StringLitExpr>(event);
  if (literal == nullptr) {
    return ok(name);
  }
  auto written = ToUtf8::toUtf8(literal->value);
  const auto *known = ProcessEventTable::find(written);
  bool isSignal = known != nullptr && known->kind == ProcessEventKind::SIGNAL;
  if (!isSignal) {
    auto message = ErrorTextProcessCall::signal(name, written);

    return err(ProcessCheckerListenedSignalError{
        .code = ProcessCheckerListenedSignalErrorCode::NOT_A_SIGNAL,
        .message = message,
    });
  }

  return ok(name);
}

Expr *ProcessChecker::checkSignalCall(CallExpr *call, MemberExpr *member, ProcessMember method) {
  call->type = Type::error();
  auto arity = this->argumentChecker->checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);

    return call;
  }
  auto listened = this->checkListenedSignal(member->name, call->args[0]);
  if (listened.isErr()) {
    this->state.getDiagnostics().error(call->args[0]->loc(), listened.error().message);
    call->args[1] = this->exprChecker->checkExpr(call->args[1]);

    return call;
  }
  auto *listener = this->exprChecker->checkExpr(call->args[1]);
  call->args[1] = listener;
  if (listener->type->isError()) {
    return call;
  }
  auto function = this->signalFunction(method, listener->type);
  if (method != ProcessMember::OFF) {
    bool isOnce = method == ProcessMember::ONCE;
    auto *once = this->state.getAst().make<BoolLitExpr>(call->loc(), isOnce);
    once->type = Type::boolTy();
    call->args.push_back(once);
  }
  std::vector<bool> given(call->args.size(), true);
  given[0] = false;
  llvm::StringRef functionName = ProcessFunctionTable::getName(function);

  return this->eventTargetChecker->callLibraryFunction(call, functionName, {}, given,
                                                       member->loc());
}

ProcessFunction ProcessChecker::signalFunction(ProcessMember method, const Type *listener) const {
  bool isWaiting = listener->isFunction() && listener->ret()->isPromise();
  if (method == ProcessMember::OFF) {
    return isWaiting ? ProcessFunction::OFF_ASYNC : ProcessFunction::OFF;
  }

  return isWaiting ? ProcessFunction::ON_ASYNC : ProcessFunction::ON;
}

Expr *ProcessChecker::checkProcessMember(MemberExpr *member) {
  if (this->isProcessMember(member->object, ProcessMember::ENV)) {
    return this->envValue(member);
  }
  auto stream = this->outputStream(member->object);
  if (stream.has_value()) {
    return this->checkStdStreamMember(member, *stream);
  }
  if (this->callChecker->isGlobalName(member->object, GlobalName::DATE)) {
    return this->dateChecker->checkDateStaticMember(member);
  }
  if (this->callChecker->isGlobalName(member->object, GlobalName::PERFORMANCE)) {
    return this->rejectPerformanceMember(member);
  }
  if (!this->callChecker->isGlobalName(member->object, GlobalName::PROCESS)) {
    return nullptr;
  }

  return this->checkProcessField(member);
}

Expr *ProcessChecker::envValue(MemberExpr *member) {
  auto name = ToUtf16::toUtf16(member->name);
  auto *key = this->state.getAst().make<StringLitExpr>(member->loc(), name);
  key->type = Type::string();
  const auto *value = Type::optional(Type::string());

  return this->builtinCall(member->loc(), member, Builtin::ProcessEnv, {key}, value);
}

Expr *ProcessChecker::rejectPerformanceMember(MemberExpr *member) {
  bool isNow = this->callChecker->isGlobalMember(member, GlobalMember::PERFORMANCE_NOW);
  std::string message =
      isNow ? ErrorTextProcessMember::perfNow() : ErrorTextProcessMember::perfOther(member->name);
  this->state.getDiagnostics().error(member->loc(), message);
  member->type = Type::error();

  return member;
}

Expr *ProcessChecker::checkProcessField(MemberExpr *member) {
  auto found = ProcessMemberTable::find(member->name);
  auto *source = this->isStream(found) ? this->streamSources->processStream(member) : nullptr;
  if (source != nullptr) {
    return source;
  }
  Expr *value = found.has_value() ? this->processValue(member, *found) : nullptr;
  if (value != nullptr) {
    return value;
  }
  member->type = Type::error();
  auto message = this->processMemberMessage(member->name, found);
  this->state.getDiagnostics().error(member->loc(), message);

  return member;
}

bool ProcessChecker::isStream(std::optional<ProcessMember> found) const {
  return found == ProcessMember::STDIN || found == ProcessMember::STDOUT ||
         found == ProcessMember::STDERR;
}

Expr *ProcessChecker::checkStdStreamMember(MemberExpr *member, ProcessMember stream) {
  bool isStdout = stream == ProcessMember::STDOUT;
  auto found = StdStreamMemberTable::find(member->name);
  bool isKnown = found == StdStreamMember::IS_TTY || found == StdStreamMember::COLUMNS ||
                 found == StdStreamMember::ROWS;
  if (!isKnown && this->hasStreamLibrary()) {
    return nullptr;
  }
  if (found == StdStreamMember::IS_TTY) {
    uint64_t descriptor = isStdout ? kStdoutDescriptor : kStderrDescriptor;
    auto *fd = this->state.getAst().make<IntLitExpr>(member->loc(), descriptor);
    fd->type = Type::intTy();

    return this->builtinCall(member->loc(), member, Builtin::ProcessIsTTY, {fd}, Type::boolTy());
  }
  auto shown = ErrorTextProcessStream::name(isStdout);
  std::string message = found == StdStreamMember::WRITE
                            ? ErrorTextProcessMember::writeCall(shown)
                            : ErrorTextProcessMember::streamPart(shown, member->name);
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
                                                 std::optional<ProcessMember> found) const {
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
  if (!this->isProcessMember(index->object, ProcessMember::ENV)) {
    return nullptr;
  }
  auto *key = this->argumentChecker->checkArg(index->index, Type::string());
  const auto *value = Type::optional(Type::string());

  return this->builtinCall(index->loc(), index->object, Builtin::ProcessEnv, {key}, value);
}

Expr *ProcessChecker::checkProcessAssign(AssignExpr *assign) {
  auto *member = dyn_cast<MemberExpr>(assign->target);
  if (member != nullptr && this->callChecker->isGlobalName(member->object, GlobalName::PROCESS)) {
    return this->checkFieldAssign(assign, member);
  }
  Expr *object = this->targetObject(assign->target);
  if (object != nullptr && this->outputStream(object).has_value()) {
    return this->rejectStreamAssign(assign, cast<MemberExpr>(object));
  }

  return nullptr;
}

Expr *ProcessChecker::targetObject(const Expr *target) const {
  auto *member = dyn_cast<MemberExpr>(target);
  auto *index = dyn_cast<IndexExpr>(target);
  if (member != nullptr) {
    return member->object;
  }
  if (index != nullptr) {
    return index->object;
  }

  return nullptr;
}

Expr *ProcessChecker::checkFieldAssign(AssignExpr *assign, const MemberExpr *member) {
  auto found = ProcessMemberTable::find(member->name);
  if (found != ProcessMember::EXIT_CODE) {
    auto message = ErrorTextProcessAssign::assigned(member->name);
    this->state.getDiagnostics().error(assign->target->loc(), message);

    return this->failedAssign(assign);
  }
  if (assign->op.has_value()) {
    auto message = ErrorTextProcessAssign::exitCode();
    this->state.getDiagnostics().error(assign->loc(), message);

    return this->failedAssign(assign);
  }
  auto *code = this->checkExitCode(assign->value);

  return this->builtinCall(assign->loc(), assign->target, Builtin::ProcessSetExitCode, {code},
                           Type::voidTy());
}

Expr *ProcessChecker::rejectStreamAssign(AssignExpr *assign, const MemberExpr *stream) {
  auto message = ErrorTextProcessAssign::changed(stream->name);
  this->state.getDiagnostics().error(assign->target->loc(), message);

  return this->failedAssign(assign);
}

Expr *ProcessChecker::failedAssign(AssignExpr *assign) {
  assign->value = this->exprChecker->checkExpr(assign->value);
  assign->type = Type::error();

  return assign;
}

}
