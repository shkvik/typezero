#include "compiler/semantic/library_v2/process/library_process_registry.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/process/process_event_table/process_event_table.h"
#include "compiler/program/library/process/process_function_table/process_function_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

namespace {

constexpr uint64_t kStdoutDescriptor = 1;
constexpr uint64_t kStderrDescriptor = 2;

}

LibraryProcessRegistry::LibraryProcessRegistry(SemanticState &owner)
    : state(owner) {}

std::optional<ProcessMember> LibraryProcessRegistry::outputStream(const Expr *expr) const {
  if (this->state.library.common.isProcessMember(expr, ProcessMember::STDOUT)) {
    return ProcessMember::STDOUT;
  }
  if (this->state.library.common.isProcessMember(expr, ProcessMember::STDERR)) {
    return ProcessMember::STDERR;
  }

  return std::nullopt;
}

bool LibraryProcessRegistry::hasStreamLibrary() const {
  llvm::StringRef name = ProcessFunctionTable::getName(ProcessFunction::STDOUT);

  return this->state.library.declarations.findLibraryFunction(name) != nullptr;
}

bool LibraryProcessRegistry::hasSignalLibrary() const {
  llvm::StringRef name = ProcessFunctionTable::getName(ProcessFunction::ON);

  return this->state.library.declarations.findLibraryFunction(name) != nullptr;
}

void LibraryProcessRegistry::exitCodeSlot(const ExitCodeSlot &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterExitCode{.slot = task.slot});

  return tasks.pushTask(CheckExprSlot{.slot = task.slot, .expected = Type::intTy()});
}

void LibraryProcessRegistry::afterExitCode(const AfterExitCode &task) {
  auto &tasks = this->state.walk.tasks;
  const auto *optionalInt = Type::optional(Type::intTy());
  const Type *type = (*task.slot)->type;
  if (type->isOptional() || type->isUndefined()) {
    return tasks.pushTask(CoerceSlot{.slot = task.slot, .to = optionalInt, .what = ""});
  }
  tasks.pushTask(ConvertSlot{.slot = task.slot, .to = optionalInt});

  return tasks.pushTask(CoerceSlot{.slot = task.slot, .to = Type::intTy(), .what = ""});
}

void LibraryProcessRegistry::checkProcessCall(const CheckProcessCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  if (this->state.isGlobalMember(member, GlobalMember::DATE_NOW)) {
    return this->checkClockNow(call, member, Builtin::DateNow, Type::intTy());
  }
  if (this->state.isGlobalName(member->object, GlobalName::DATE)) {
    return tasks.pushTask(CheckDateStatic{.call = call, .member = member});
  }
  if (this->state.isGlobalMember(member, GlobalMember::PERFORMANCE_NOW)) {
    return this->checkClockNow(call, member, Builtin::PerformanceNow, Type::doubleTy());
  }
  if (this->state.isGlobalName(member->object, GlobalName::PERFORMANCE)) {
    return this->rejectPerformanceCall(call, member);
  }
  auto stream = this->outputStream(member->object);
  if (stream.has_value()) {
    return this->checkStdStreamCall(call, member, *stream);
  }
  if (!this->state.isGlobalName(member->object, GlobalName::PROCESS)) {
    return tasks.pushChecked(nullptr);
  }
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto found = ProcessMemberTable::find(member->name);
  if (found == ProcessMember::NEXT_TICK) {
    return tasks.pushTask(CheckNextTick{.call = call, .member = member});
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

void LibraryProcessRegistry::checkClockNow(CallExpr *call, const MemberExpr *member,
                                           Builtin builtin, const Type *result) {
  auto &tasks = this->state.walk.tasks;
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }

  return this->checkNoArgCall(call, member, builtin, result);
}

void LibraryProcessRegistry::rejectPerformanceCall(CallExpr *call, const MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  llvm::StringRef object = GlobalNameTable::getName(GlobalName::PERFORMANCE);
  auto message = ErrorTextProcessCall::onlyNow(object, member->name);
  this->state.diagnostics.error(member->loc(), message);
  tasks.pushChecked(call);

  return tasks.pushTask(WalkArgsLoosely{.call = call});
}

void LibraryProcessRegistry::checkStdStreamCall(CallExpr *call, const MemberExpr *member,
                                                ProcessMember stream) {
  auto &tasks = this->state.walk.tasks;
  bool isStdout = stream == ProcessMember::STDOUT;
  auto found = StdStreamMemberTable::find(member->name);
  bool isTty = found == StdStreamMember::IS_TTY;
  if (!isTty && this->hasStreamLibrary()) {
    return tasks.pushChecked(nullptr);
  }
  tasks.pushChecked(call);
  if (found != StdStreamMember::WRITE) {
    auto shown = ErrorTextProcessStream::name(isStdout);
    std::string message = isTty ? ErrorTextProcessCall::ttyValue(shown)
                                : ErrorTextProcessCall::streamCall(shown, member->name);
    this->state.diagnostics.error(member->loc(), message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  tasks.pushTask(AfterStdStreamChunk{.call = call, .isStdout = isStdout});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = Type::string()});
}

void LibraryProcessRegistry::afterStdStreamChunk(const AfterStdStreamChunk &task) {
  CallExpr *call = task.call;
  Builtin builtin = task.isStdout ? Builtin::ProcessStdoutWrite : Builtin::ProcessStderrWrite;
  call->target = BuiltinCall{.builtin = builtin};
  call->type = Type::boolTy();
  const Type *type = call->args[0]->type;
  if (type->isBuffer() || type->bytes() == Type::Bytes::Uint8) {
    return;
  }

  return this->state.walk.tasks.pushTask(
      CoerceSlot{.slot = &call->args[0], .to = Type::string(), .what = ""});
}

void LibraryProcessRegistry::checkExit(CallExpr *call, const MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  auto arity = this->state.library.common.checkArity(call, 0, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  call->target = BuiltinCall{.builtin = Builtin::ProcessExit};
  call->type = Type::voidTy();
  if (call->args.empty()) {
    return;
  }

  return tasks.pushTask(ExitCodeSlot{.slot = &call->args[0]});
}

void LibraryProcessRegistry::checkNoArgCall(CallExpr *call, const MemberExpr *member,
                                            Builtin builtin, const Type *result) {
  this->state.walk.tasks.pushChecked(call);
  auto arity = this->state.library.common.checkArity(call, 0, 0);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  call->target = BuiltinCall{.builtin = builtin};
  call->type = result;
}

void LibraryProcessRegistry::checkProcessListenerCall(CallExpr *call, MemberExpr *member,
                                                      std::optional<ProcessMember> found) {
  auto &tasks = this->state.walk.tasks;
  if (this->isSubscribing(found) && this->isThrownEvent(call)) {
    auto message = ErrorTextProcessEvent::noThrown();
    this->state.diagnostics.error(call->args[0]->loc(), message);
    tasks.pushChecked(call);

    return tasks.pushTask(MarkCallFailed{.call = call});
  }
  if (this->isSignalMethod(found) && this->hasSignalLibrary()) {
    return this->checkSignalCall(call, member, *found);
  }
  std::string message = this->isProperty(found) ? ErrorTextProcessCall::notFunc(member->name)
                                                : ErrorTextProcessCall::notYet(member->name);
  this->state.diagnostics.error(member->loc(), message);
  tasks.pushChecked(call);

  return tasks.pushTask(WalkArgsLoosely{.call = call});
}

bool LibraryProcessRegistry::isSubscribing(std::optional<ProcessMember> found) const {
  return found == ProcessMember::ON || found == ProcessMember::ONCE ||
         found == ProcessMember::ADD_LISTENER || found == ProcessMember::PREPEND_LISTENER;
}

bool LibraryProcessRegistry::isSignalMethod(std::optional<ProcessMember> found) const {
  return found == ProcessMember::ON || found == ProcessMember::ONCE || found == ProcessMember::OFF;
}

bool LibraryProcessRegistry::isProperty(std::optional<ProcessMember> found) const {
  bool isValue = found == ProcessMember::ARGV || found == ProcessMember::ENV ||
                 found == ProcessMember::EXIT_CODE || found == ProcessMember::EXEC_PATH ||
                 found == ProcessMember::PLATFORM;

  return isValue || found == ProcessMember::STDOUT || found == ProcessMember::STDERR;
}

bool LibraryProcessRegistry::isThrownEvent(const CallExpr *call) const {
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

LibraryProcessRegistry::ResultListenedSignal
LibraryProcessRegistry::checkListenedSignal(llvm::StringRef name, const Expr *event) const {
  auto *literal = dyn_cast<StringLitExpr>(event);
  if (literal == nullptr) {
    return ok(name);
  }
  auto written = ToUtf8::toUtf8(literal->value);
  const auto *known = ProcessEventTable::find(written);
  bool isSignal = known != nullptr && known->kind == ProcessEventKind::SIGNAL;
  if (!isSignal) {
    auto message = ErrorTextProcessCall::signal(name, written);

    return err(LibraryProcessRegistryListenedSignalError{
        .code = LibraryProcessRegistryListenedSignalErrorCode::NOT_A_SIGNAL,
        .message = message,
    });
  }

  return ok(name);
}

void LibraryProcessRegistry::checkSignalCall(CallExpr *call, MemberExpr *member,
                                             ProcessMember method) {
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  auto arity = this->state.library.common.checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.diagnostics.error(member->loc(), arity.error().message);

    return tasks.pushChecked(call);
  }
  auto listened = this->checkListenedSignal(member->name, call->args[0]);
  if (listened.isErr()) {
    this->state.diagnostics.error(call->args[0]->loc(), listened.error().message);
    tasks.pushChecked(call);

    return tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = nullptr});
  }
  tasks.pushTask(AfterSignalListener{.call = call, .member = member, .method = method});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = nullptr});
}

void LibraryProcessRegistry::afterSignalListener(const AfterSignalListener &task) {
  CallExpr *call = task.call;
  ProcessMember method = task.method;
  auto &tasks = this->state.walk.tasks;
  const Expr *listener = call->args[1];
  if (listener->type->isError()) {
    return tasks.pushChecked(call);
  }
  auto function = this->signalFunction(method, listener->type);
  if (method != ProcessMember::OFF) {
    bool isOnce = method == ProcessMember::ONCE;
    auto *once = this->state.ast.make<BoolLitExpr>(call->loc(), isOnce);
    once->type = Type::boolTy();
    call->args.push_back(once);
  }
  auto given = std::vector<bool>(call->args.size(), true);
  given[0] = false;
  llvm::StringRef functionName = ProcessFunctionTable::getName(function);

  return tasks.pushTask(CallLibraryFunction{
      .call = call,
      .name = functionName,
      .bound = {},
      .given = std::move(given),
      .loc = task.member->loc(),
  });
}

ProcessFunction LibraryProcessRegistry::signalFunction(ProcessMember method,
                                                       const Type *listener) const {
  bool isWaiting = listener->isFunction() && listener->ret()->isPromise();
  if (method == ProcessMember::OFF) {
    return isWaiting ? ProcessFunction::OFF_ASYNC : ProcessFunction::OFF;
  }

  return isWaiting ? ProcessFunction::ON_ASYNC : ProcessFunction::ON;
}

void LibraryProcessRegistry::checkProcessMember(const CheckProcessMember &task) {
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  if (this->state.library.common.isProcessMember(member->object, ProcessMember::ENV)) {
    auto *value = this->envValue(member);

    return tasks.pushChecked(value);
  }
  auto stream = this->outputStream(member->object);
  if (stream.has_value()) {
    auto *checked = this->checkStdStreamMember(member, *stream);

    return tasks.pushChecked(checked);
  }
  if (this->state.isGlobalName(member->object, GlobalName::DATE)) {
    return tasks.pushTask(CheckDateStaticMember{.member = member});
  }
  if (this->state.isGlobalName(member->object, GlobalName::PERFORMANCE)) {
    auto *rejected = this->rejectPerformanceMember(member);

    return tasks.pushChecked(rejected);
  }
  if (!this->state.isGlobalName(member->object, GlobalName::PROCESS)) {
    return tasks.pushChecked(nullptr);
  }
  auto found = ProcessMemberTable::find(member->name);
  if (!this->isStream(found)) {
    return this->checkProcessValue(member, found);
  }
  tasks.pushTask(AfterProcessStream{.member = member});

  return tasks.pushTask(ProcessStream{.member = member});
}

void LibraryProcessRegistry::afterProcessStream(const AfterProcessStream &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *source = tasks.takeChecked();
  if (source != nullptr) {
    return tasks.pushChecked(source);
  }
  auto found = ProcessMemberTable::find(task.member->name);

  return this->checkProcessValue(task.member, found);
}

Expr *LibraryProcessRegistry::envValue(MemberExpr *member) {
  auto name = ToUtf16::toUtf16(member->name);
  auto *key = this->state.ast.make<StringLitExpr>(member->loc(), name);
  key->type = Type::string();
  const auto *value = Type::optional(Type::string());

  return this->state.library.common.builtinCall(member->loc(), member, Builtin::ProcessEnv, {key},
                                                value);
}

Expr *LibraryProcessRegistry::rejectPerformanceMember(MemberExpr *member) {
  bool isNow = this->state.isGlobalMember(member, GlobalMember::PERFORMANCE_NOW);
  std::string message =
      isNow ? ErrorTextProcessMember::perfNow() : ErrorTextProcessMember::perfOther(member->name);
  this->state.diagnostics.error(member->loc(), message);
  member->type = Type::error();

  return member;
}

void LibraryProcessRegistry::checkProcessValue(MemberExpr *member,
                                               std::optional<ProcessMember> found) {
  auto &tasks = this->state.walk.tasks;
  Expr *value = found.has_value() ? this->processValue(member, *found) : nullptr;
  if (value != nullptr) {
    return tasks.pushChecked(value);
  }
  member->type = Type::error();
  auto message = this->processMemberMessage(member->name, found);
  this->state.diagnostics.error(member->loc(), message);

  return tasks.pushChecked(member);
}

bool LibraryProcessRegistry::isStream(std::optional<ProcessMember> found) const {
  return found == ProcessMember::STDIN || found == ProcessMember::STDOUT ||
         found == ProcessMember::STDERR;
}

Expr *LibraryProcessRegistry::checkStdStreamMember(MemberExpr *member, ProcessMember stream) {
  bool isStdout = stream == ProcessMember::STDOUT;
  auto found = StdStreamMemberTable::find(member->name);
  bool isKnown = found == StdStreamMember::IS_TTY || found == StdStreamMember::COLUMNS ||
                 found == StdStreamMember::ROWS;
  if (!isKnown && this->hasStreamLibrary()) {
    return nullptr;
  }
  if (found == StdStreamMember::IS_TTY) {
    uint64_t descriptor = isStdout ? kStdoutDescriptor : kStderrDescriptor;
    auto *fd = this->state.ast.make<IntLitExpr>(member->loc(), descriptor);
    fd->type = Type::intTy();

    return this->state.library.common.builtinCall(member->loc(), member, Builtin::ProcessIsTTY,
                                                  {fd}, Type::boolTy());
  }
  auto shown = ErrorTextProcessStream::name(isStdout);
  std::string message = found == StdStreamMember::WRITE
                            ? ErrorTextProcessMember::writeCall(shown)
                            : ErrorTextProcessMember::streamPart(shown, member->name);
  this->state.diagnostics.error(member->loc(), message);
  member->type = Type::error();

  return member;
}

Expr *LibraryProcessRegistry::processValue(MemberExpr *member, ProcessMember found) {
  llvm::SMLoc loc = member->loc();
  switch (found) {
    case ProcessMember::ARGV:
      return this->state.library.common.builtinCall(loc, member, Builtin::ProcessArgv, {},
                                                    Type::array(Type::string()));
    case ProcessMember::EXIT_CODE:
      return this->state.library.common.builtinCall(loc, member, Builtin::ProcessExitCode, {},
                                                    Type::optional(Type::intTy()));
    case ProcessMember::EXEC_PATH:
      return this->state.library.common.builtinCall(loc, member, Builtin::ProcessExecPath, {},
                                                    Type::string());
    case ProcessMember::PLATFORM:
      return this->state.library.common.builtinCall(loc, member, Builtin::ProcessPlatform, {},
                                                    Type::string());
    case ProcessMember::PID:
      return this->state.library.common.builtinCall(loc, member, Builtin::ProcessPid, {},
                                                    Type::intTy());
    case ProcessMember::ENV:
      return this->state.library.common.builtinCall(
          loc, member, Builtin::EnvMap, {},
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

std::string LibraryProcessRegistry::processMemberMessage(llvm::StringRef name,
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

void LibraryProcessRegistry::checkProcessEnvIndex(const CheckProcessEnvIndex &task) {
  IndexExpr *index = task.index;
  auto &tasks = this->state.walk.tasks;
  if (!this->state.library.common.isProcessMember(index->object, ProcessMember::ENV)) {
    return tasks.pushChecked(nullptr);
  }
  tasks.pushTask(AfterEnvIndexKey{.index = index});

  return tasks.pushTask(CheckArgSlot{.slot = &index->index, .to = Type::string()});
}

void LibraryProcessRegistry::afterEnvIndexKey(const AfterEnvIndexKey &task) {
  IndexExpr *index = task.index;
  const auto *value = Type::optional(Type::string());
  auto *made = this->state.library.common.builtinCall(index->loc(), index->object,
                                                      Builtin::ProcessEnv, {index->index}, value);

  return this->state.walk.tasks.pushChecked(made);
}

void LibraryProcessRegistry::checkProcessAssign(const CheckProcessAssign &task) {
  AssignExpr *assign = task.assign;
  auto *member = dyn_cast<MemberExpr>(assign->target);
  if (member != nullptr && this->state.isGlobalName(member->object, GlobalName::PROCESS)) {
    return this->checkFieldAssign(assign, member);
  }
  Expr *object = this->targetObject(assign->target);
  if (object != nullptr && this->outputStream(object).has_value()) {
    return this->rejectStreamAssign(assign, cast<MemberExpr>(object));
  }

  return this->state.walk.tasks.pushChecked(nullptr);
}

Expr *LibraryProcessRegistry::targetObject(const Expr *target) const {
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

void LibraryProcessRegistry::checkFieldAssign(AssignExpr *assign, const MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  auto found = ProcessMemberTable::find(member->name);
  if (found != ProcessMember::EXIT_CODE) {
    auto message = ErrorTextProcessAssign::assigned(member->name);
    this->state.diagnostics.error(assign->target->loc(), message);

    return this->failAssign(assign);
  }
  if (assign->op.has_value()) {
    auto message = ErrorTextProcessAssign::exitCode();
    this->state.diagnostics.error(assign->loc(), message);

    return this->failAssign(assign);
  }
  tasks.pushTask(AfterAssignedExitCode{.assign = assign});

  return tasks.pushTask(ExitCodeSlot{.slot = &assign->value});
}

void LibraryProcessRegistry::afterAssignedExitCode(const AfterAssignedExitCode &task) {
  AssignExpr *assign = task.assign;
  auto *made = this->state.library.common.builtinCall(
      assign->loc(), assign->target, Builtin::ProcessSetExitCode, {assign->value}, Type::voidTy());

  return this->state.walk.tasks.pushChecked(made);
}

void LibraryProcessRegistry::rejectStreamAssign(AssignExpr *assign, const MemberExpr *stream) {
  auto message = ErrorTextProcessAssign::changed(stream->name);
  this->state.diagnostics.error(assign->target->loc(), message);

  return this->failAssign(assign);
}

void LibraryProcessRegistry::failAssign(AssignExpr *assign) {
  auto &tasks = this->state.walk.tasks;
  assign->type = Type::error();
  tasks.pushChecked(assign);

  return tasks.pushTask(CheckExprSlot{.slot = &assign->value, .expected = nullptr});
}

}
