#include "compiler/semantic/library_v2/child_process/library_child_process_registry.h"
#include "compiler/program/library/bytes/encoding_name_table/encoding_name_table.h"
#include "compiler/program/library/child_process/child_error_names/child_error_names.h"
#include "compiler/program/library/child_process/child_signal_table/child_signal_table.h"
#include "compiler/program/library/child_process/child_stdio_table/child_stdio_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include <iterator>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

const NodeOption kChildOptions[] = {NodeOption::WORKING_DIRECTORY,
                                    NodeOption::ENVIRONMENT,
                                    NodeOption::INPUT,
                                    NodeOption::STDIO,
                                    NodeOption::ENCODING,
                                    NodeOption::TIMEOUT,
                                    NodeOption::KILL_SIGNAL,
                                    NodeOption::MAX_BUFFER,
                                    NodeOption::SHELL,
                                    NodeOption::WINDOWS_HIDE,
                                    NodeOption::WINDOWS_VERBATIM_ARGUMENTS,
                                    NodeOption::DETACHED,
                                    NodeOption::ARGV0,
                                    NodeOption::USER_ID,
                                    NodeOption::GROUP_ID};

struct ChildPlainOption {
  size_t argIndex;
  NodeOption option;
};

const ChildPlainOption kPlainOptions[] = {
    {.argIndex = ChildCwd, .option = NodeOption::WORKING_DIRECTORY},
    {.argIndex = ChildEnv, .option = NodeOption::ENVIRONMENT},
    {.argIndex = ChildTimeout, .option = NodeOption::TIMEOUT},
    {.argIndex = ChildMaxBuffer, .option = NodeOption::MAX_BUFFER},
    {.argIndex = ChildWindowsHide, .option = NodeOption::WINDOWS_HIDE},
    {.argIndex = ChildVerbatim, .option = NodeOption::WINDOWS_VERBATIM_ARGUMENTS},
    {.argIndex = ChildDetached, .option = NodeOption::DETACHED},
    {.argIndex = ChildArgv0, .option = NodeOption::ARGV0},
    {.argIndex = ChildUid, .option = NodeOption::USER_ID},
    {.argIndex = ChildGid, .option = NodeOption::GROUP_ID},
};

constexpr size_t kStdioCount = 3;

}

LibraryChildProcessRegistry::LibraryChildProcessRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryChildProcessRegistry::checkChildCall(const CheckChildCall &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  auto function = ChildFunctionTable::find(task.functionName);
  if (!function.has_value()) {
    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  bool isExec = function == ChildFunction::EXEC_SYNC;
  unsigned maxArgs = isExec ? 2 : 3;
  auto arity = this->state.library.common.checkArity(call, 1, maxArgs);
  if (arity.isErr()) {
    return this->state.diagnostics.error(task.loc, arity.error().message);
  }
  auto run = ChildRunDraft{
      .call = call,
      .function = *function,
      .functionName = task.functionName,
      .loc = task.loc,
      .isExec = isExec,
      .isFailed = false,
      .args = std::vector<Expr *>(ChildArgCount, nullptr),
      .optionsAt = 1,
      .encoding = ChildBytes,
      .stdio = {ChildPipe, ChildPipe, ChildPipe},
      .isStdioGiven = false,
      .isViaShell = isExec,
  };
  tasks.pushTask(AfterChildFile{.run = std::move(run)});

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryChildProcessRegistry::afterChildFile(const AfterChildFile &task) {
  ChildRunDraft run = task.run;
  CallExpr *call = run.call;
  auto &tasks = this->state.walk.tasks;
  Expr *file = call->args[0];
  run.args[ChildFile] = file;
  auto named = this->checkFileName(file, run.isExec);
  if (named.isErr()) {
    this->state.diagnostics.error(file->loc(), named.error().message);
    run.isFailed = true;
  }
  bool isListGiven = !run.isExec && call->args.size() > 1 && !isa<ObjectLitExpr>(call->args[1]);
  if (!run.isExec && !isListGiven && call->args.size() > 2) {
    auto message = ErrorTextChildRun::optionsAt(run.functionName);
    return this->state.diagnostics.error(call->args[2]->loc(), message);
  }
  run.optionsAt = isListGiven ? 2 : 1;
  if (!isListGiven || isa<UndefinedExpr>(call->args[1])) {
    return this->readChildOptions(run);
  }
  this->walkZeroArguments(run, call->args[1]);
  tasks.pushTask(AfterChildArgs{.run = std::move(run)});

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[1], .to = Type::array(Type::string())});
}

void LibraryChildProcessRegistry::afterChildArgs(const AfterChildArgs &task) {
  ChildRunDraft run = task.run;
  run.args[ChildArgs] = run.call->args[1];

  return this->readChildOptions(run);
}

void LibraryChildProcessRegistry::readChildOptions(ChildRunDraft &run) {
  auto &tasks = this->state.walk.tasks;
  if (run.args[ChildArgs] == nullptr) {
    auto *none = this->state.ast.make<ArrayLitExpr>(run.loc);
    none->type = Type::array(Type::string());
    run.args[ChildArgs] = none;
  }
  auto read = ReadNodeOptions{
      .call = run.call,
      .index = run.optionsAt,
      .functionName = run.functionName,
      .allowed = std::vector<NodeOption>(std::begin(kChildOptions), std::end(kChildOptions)),
  };
  tasks.pushTask(AfterChildOptions{.run = std::move(run)});

  return tasks.pushTask(std::move(read));
}

void LibraryChildProcessRegistry::afterChildOptions(const AfterChildOptions &task) {
  ChildRunDraft run = task.run;
  bool isRead = this->state.walk.tasks.takeVerdict();
  if (!isRead) {
    return;
  }

  return this->walkEncodingOption(run);
}

void LibraryChildProcessRegistry::walkEncodingOption(ChildRunDraft &run) {
  Expr *value = this->optionOf(run, NodeOption::ENCODING);
  if (value == nullptr || isa<UndefinedExpr>(value)) {
    return this->walkStdioOption(run);
  }
  auto *literal = dyn_cast<StringLitExpr>(value);
  if (literal == nullptr) {
    auto message = ErrorTextChildOption::encoding(run.functionName);
    this->state.diagnostics.error(value->loc(), message);
    run.isFailed = true;

    return this->pushDroppedCheck(value, WalkChildStdio{.run = std::move(run)});
  }
  auto written = ToUtf8::toUtf8(literal->value);
  if (EncodingNameTable::isBufferMarker(written)) {
    return this->walkStdioOption(run);
  }
  literal->type = Type::string();
  auto encoding = this->state.library.common.checkKnownEncoding(written);
  if (encoding.isErr()) {
    this->state.diagnostics.error(literal->loc(), encoding.error().message);
    literal->type = Type::error();
    run.isFailed = true;

    return this->walkStdioOption(run);
  }
  run.encoding = 1 + unsigned(encoding.value());

  return this->walkStdioOption(run);
}

void LibraryChildProcessRegistry::walkChildStdio(const WalkChildStdio &task) {
  ChildRunDraft run = task.run;

  return this->walkStdioOption(run);
}

void LibraryChildProcessRegistry::walkStdioOption(ChildRunDraft &run) {
  auto &tasks = this->state.walk.tasks;
  Expr *value = this->optionOf(run, NodeOption::STDIO);
  if (value == nullptr || isa<UndefinedExpr>(value)) {
    return this->walkOptionLimits(run);
  }
  run.isStdioGiven = true;
  auto *list = dyn_cast<ArrayLitExpr>(value);
  if (list != nullptr && list->elements.size() > kStdioCount) {
    auto message = ErrorTextChildStdio::three();
    this->state.diagnostics.error(list->elements[kStdioCount].expr->loc(), message);
    run.isFailed = true;
  }
  if (list != nullptr) {
    return tasks.pushTask(NextStdioEntry{.run = std::move(run), .index = 0});
  }
  if (isa<StringLitExpr>(value)) {
    this->walkStdioText(run, value);

    return this->walkOptionLimits(run);
  }
  auto message = ErrorTextChildOption::stdio(run.functionName);
  this->state.diagnostics.error(value->loc(), message);
  run.isFailed = true;

  return this->pushDroppedCheck(value, WalkChildLimits{.run = std::move(run)});
}

void LibraryChildProcessRegistry::nextStdioEntry(const NextStdioEntry &task) {
  ChildRunDraft run = task.run;
  size_t index = task.index;
  auto *list = cast<ArrayLitExpr>(this->optionOf(run, NodeOption::STDIO));
  if (index >= list->elements.size() || index >= kStdioCount) {
    return this->walkOptionLimits(run);
  }
  const ArrayLitExpr::Element &element = list->elements[index];
  auto kind = this->checkStdioEntry(element, unsigned(index));
  if (kind.isOk()) {
    run.stdio[index] = kind.value();
  } else {
    this->state.diagnostics.error(element.expr->loc(), kind.error().message);
    run.isFailed = true;
  }
  bool isUnchecked = kind.isErr() && kind.error().code ==
                                         LibraryChildProcessRegistryStdioKindErrorCode::NOT_LITERAL;
  auto next = NextStdioEntry{.run = std::move(run), .index = index + 1};
  if (isUnchecked) {
    return this->pushDroppedCheck(element.expr, std::move(next));
  }

  return this->state.walk.tasks.pushTask(std::move(next));
}

void LibraryChildProcessRegistry::walkStdioText(ChildRunDraft &run, Expr *value) {
  this->typeStdioLiteral(value);
  for (unsigned index = 0; index < kStdioCount; ++index) {
    auto kind = this->checkStdioKind(value, index);
    if (kind.isErr()) {
      this->state.diagnostics.error(value->loc(), kind.error().message);
      run.isFailed = true;

      return;
    }
    run.stdio[index] = kind.value();
  }
}

void LibraryChildProcessRegistry::walkChildLimits(const WalkChildLimits &task) {
  ChildRunDraft run = task.run;

  return this->walkOptionLimits(run);
}

void LibraryChildProcessRegistry::walkOptionLimits(ChildRunDraft &run) {
  Expr *directory = this->optionOf(run, NodeOption::WORKING_DIRECTORY);
  if (this->hasZero(directory)) {
    auto message = ErrorTextChildRun::zeroCwd();
    this->state.diagnostics.error(directory->loc(), message);
    run.isFailed = true;
  }
  for (NodeOption limit : {NodeOption::TIMEOUT, NodeOption::MAX_BUFFER}) {
    Expr *value = this->optionOf(run, limit);
    if (this->isNegativeLiteral(value)) {
      auto name = NodeOptionTable::getName(limit);
      auto message = ErrorTextChildRun::negative(name);
      this->state.diagnostics.error(value->loc(), message);
      run.isFailed = true;
    }
  }
  bool isWindows = NodeCheckHelpers::isTargetWindows();
  for (NodeOption id : {NodeOption::USER_ID, NodeOption::GROUP_ID}) {
    Expr *value = this->optionOf(run, id);
    bool isGiven = value != nullptr && !isa<UndefinedExpr>(value);
    if (isWindows && isGiven) {
      auto name = NodeOptionTable::getName(id);
      auto message = ErrorTextChildRun::unixId(name);
      this->state.diagnostics.error(value->loc(), message);
      run.isFailed = true;
    }
  }
  auto *signal = dyn_cast_or_null<StringLitExpr>(this->optionOf(run, NodeOption::KILL_SIGNAL));
  auto written = signal != nullptr ? ToUtf8::toUtf8(signal->value) : std::string();
  if (signal != nullptr && !ChildSignalTable::isKnown(written, isWindows)) {
    auto message = ErrorTextChildRun::noSignal(written);
    this->state.diagnostics.error(signal->loc(), message);
    run.isFailed = true;
  }

  return this->state.walk.tasks.pushTask(NextChildOption{.run = std::move(run), .index = 0});
}

void LibraryChildProcessRegistry::nextChildOption(const NextChildOption &task) {
  ChildRunDraft run = task.run;
  size_t index = task.index;
  auto &tasks = this->state.walk.tasks;
  if (index == std::size(kPlainOptions)) {
    return this->walkInputOption(run);
  }
  NodeOption option = kPlainOptions[index].option;
  const Type *type = this->plainOptionType(option);
  Expr *value = this->optionOf(run, option);
  llvm::SMLoc loc = run.loc;
  tasks.pushTask(AfterChildOption{.run = std::move(run), .index = index});
  if (value == nullptr) {
    return this->pushAbsentValue(loc, type);
  }
  const auto *optional = Type::optional(type);
  tasks.pushTask(Coerce{.to = optional, .what = ""});

  return tasks.pushTask(CheckExpr{.expr = value, .expected = optional});
}

void LibraryChildProcessRegistry::afterChildOption(const AfterChildOption &task) {
  ChildRunDraft run = task.run;
  size_t index = task.index;
  run.args[kPlainOptions[index].argIndex] = this->state.walk.tasks.takeChecked();

  return this->state.walk.tasks.pushTask(
      NextChildOption{.run = std::move(run), .index = index + 1});
}

void LibraryChildProcessRegistry::walkInputOption(ChildRunDraft &run) {
  auto &tasks = this->state.walk.tasks;
  Expr *value = this->optionOf(run, NodeOption::INPUT);
  llvm::SMLoc loc = run.loc;
  if (value == nullptr) {
    tasks.pushTask(FinishChildInput{.run = std::move(run), .isGiven = false});

    return this->pushAbsentValue(loc, Type::string());
  }
  tasks.pushTask(AfterChildInput{.run = std::move(run)});

  return tasks.pushTask(CheckExpr{.expr = value, .expected = Type::string()});
}

void LibraryChildProcessRegistry::afterChildInput(const AfterChildInput &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *input = tasks.takeChecked();
  bool isLiteral = input->type->nonOptional()->isStringLiteral();
  tasks.pushTask(CheckChildInput{.run = task.run});
  if (isLiteral) {
    tasks.pushTask(Coerce{.to = Type::string(), .what = ""});
  }

  return tasks.pushChecked(input);
}

void LibraryChildProcessRegistry::checkChildInput(const CheckChildInput &task) {
  ChildRunDraft run = task.run;
  auto &tasks = this->state.walk.tasks;
  Expr *input = tasks.takeChecked();
  const Type *type = input->type->nonOptional();
  if (type->isError()) {
    run.isFailed = true;

    return this->walkKillSignalOption(run);
  }
  if (!type->isString() && !type->isByteView()) {
    auto message = ErrorTextChildOption::input(run.functionName, input->type->name());
    this->state.diagnostics.error(input->loc(), message);
    run.isFailed = true;

    return this->walkKillSignalOption(run);
  }
  tasks.pushTask(FinishChildInput{.run = std::move(run), .isGiven = true});

  return this->pushCoercedValue(input, type);
}

void LibraryChildProcessRegistry::finishChildInput(const FinishChildInput &task) {
  ChildRunDraft run = task.run;
  Expr *input = this->state.walk.tasks.takeChecked();
  run.args[ChildInput] = input;
  Expr *encoding = this->optionOf(run, NodeOption::ENCODING);
  bool isEncodingGiven = encoding != nullptr && !isa<UndefinedExpr>(encoding);
  bool isText = input->type->nonOptional()->isString();
  if (task.isGiven && isText && run.encoding == ChildBytes && isEncodingGiven) {
    auto message = ErrorTextChildOption::bufferText();
    this->state.diagnostics.error(input->loc(), message);
    run.isFailed = true;
  }

  return this->walkKillSignalOption(run);
}

void LibraryChildProcessRegistry::walkKillSignalOption(ChildRunDraft &run) {
  auto &tasks = this->state.walk.tasks;
  Expr *value = this->optionOf(run, NodeOption::KILL_SIGNAL);
  llvm::SMLoc loc = run.loc;
  if (value == nullptr) {
    tasks.pushTask(FinishChildKillSignal{.run = std::move(run)});

    return this->pushAbsentValue(loc, Type::string());
  }
  tasks.pushTask(AfterChildKillSignal{.run = std::move(run)});

  return tasks.pushTask(CheckExpr{.expr = value, .expected = Type::string()});
}

void LibraryChildProcessRegistry::afterChildKillSignal(const AfterChildKillSignal &task) {
  ChildRunDraft run = task.run;
  auto &tasks = this->state.walk.tasks;
  Expr *signal = tasks.takeChecked();
  const Type *type = signal->type->nonOptional();
  if (type->isStringLiteral() || type->isString()) {
    tasks.pushTask(FinishChildKillSignal{.run = std::move(run)});

    return this->pushCoercedValue(signal, Type::string());
  }
  bool isNumber = type->isInt() || (type->isLiteral() && type->literalBase()->isInt());
  if (isNumber) {
    tasks.pushTask(FinishChildKillSignal{.run = std::move(run)});

    return this->pushCoercedValue(signal, Type::intTy());
  }
  if (!type->isError()) {
    auto message = ErrorTextChildOption::killSignal(signal->type->name());
    this->state.diagnostics.error(signal->loc(), message);
  }
  run.isFailed = true;

  return this->walkShellOption(run);
}

void LibraryChildProcessRegistry::finishChildKillSignal(const FinishChildKillSignal &task) {
  ChildRunDraft run = task.run;
  run.args[ChildKillSignal] = this->state.walk.tasks.takeChecked();

  return this->walkShellOption(run);
}

void LibraryChildProcessRegistry::walkShellOption(ChildRunDraft &run) {
  auto &tasks = this->state.walk.tasks;
  Expr *value = this->optionOf(run, NodeOption::SHELL);
  llvm::SMLoc loc = run.loc;
  if (value == nullptr) {
    tasks.pushTask(FinishChildShell{.run = std::move(run)});

    return this->pushAbsentValue(loc, Type::string());
  }
  auto *written = dyn_cast<BoolLitExpr>(value);
  if (written != nullptr) {
    run.isViaShell = run.isViaShell || written->value;
  }
  tasks.pushTask(AfterChildShell{.run = std::move(run)});

  return tasks.pushTask(CheckExpr{.expr = value, .expected = Type::string()});
}

void LibraryChildProcessRegistry::afterChildShell(const AfterChildShell &task) {
  ChildRunDraft run = task.run;
  auto &tasks = this->state.walk.tasks;
  Expr *shell = tasks.takeChecked();
  Expr *value = this->optionOf(run, NodeOption::SHELL);
  const Type *type = shell->type->nonOptional();
  bool isBool = type->isBool() || (type->isLiteral() && type->literalBase()->isBool());
  if (type->isStringLiteral() || type->isString()) {
    run.isViaShell = true;
    tasks.pushTask(FinishChildShell{.run = std::move(run)});

    return this->pushCoercedValue(shell, Type::string());
  }
  if (isBool && run.isExec) {
    return this->walkExecShell(run, shell);
  }
  if (isBool && !isa<BoolLitExpr>(value)) {
    auto message = ErrorTextChildOption::shellFlag(run.functionName);
    this->state.diagnostics.error(shell->loc(), message);
    run.isFailed = true;

    return this->finishChildRun(run);
  }
  if (isBool) {
    tasks.pushTask(FinishChildShell{.run = std::move(run)});

    return this->pushCoercedValue(shell, Type::boolTy());
  }
  if (!type->isError()) {
    auto message = ErrorTextChildOption::shell(shell->type->name());
    this->state.diagnostics.error(shell->loc(), message);
  }
  run.isFailed = true;

  return this->finishChildRun(run);
}

void LibraryChildProcessRegistry::walkExecShell(ChildRunDraft &run, Expr *shell) {
  auto *literal = dyn_cast<BoolLitExpr>(shell);
  if (literal == nullptr || !literal->value) {
    auto message = ErrorTextChildOption::execShell();
    this->state.diagnostics.error(shell->loc(), message);
    run.isFailed = true;

    return this->finishChildRun(run);
  }
  llvm::SMLoc loc = run.loc;
  this->state.walk.tasks.pushTask(FinishChildShell{.run = std::move(run)});

  return this->pushAbsentValue(loc, Type::string());
}

void LibraryChildProcessRegistry::finishChildShell(const FinishChildShell &task) {
  ChildRunDraft run = task.run;
  run.args[ChildShell] = this->state.walk.tasks.takeChecked();

  return this->finishChildRun(run);
}

void LibraryChildProcessRegistry::finishChildRun(ChildRunDraft &run) {
  auto &tasks = this->state.walk.tasks;
  if (run.isFailed) {
    return;
  }
  bool isText = run.encoding != ChildBytes;
  bool isSpawn = run.function == ChildFunction::SPAWN_SYNC;
  llvm::StringRef errorName = ChildErrorNames::getErrorName(run.function, isText, run.isViaShell);
  llvm::StringRef resultName = ChildErrorNames::getResultName(isText);
  tasks.pushTask(FinishChildCall{.run = std::move(run)});
  if (isSpawn) {
    tasks.pushTask(NamedModuleType{.name = resultName});
  }

  return tasks.pushTask(NamedModuleType{.name = errorName});
}

void LibraryChildProcessRegistry::finishChildCall(const FinishChildCall &task) {
  ChildRunDraft run = task.run;
  CallExpr *call = run.call;
  llvm::SMLoc loc = run.loc;
  bool isSpawn = run.function == ChildFunction::SPAWN_SYNC;
  auto types = this->state.walk.tasks.takeAnswers(isSpawn ? 2 : 1);
  const Type *failure = types[0];
  run.args[ChildStdin] = this->makeIntLiteral(loc, run.stdio[0]);
  run.args[ChildStdout] = this->makeIntLiteral(loc, run.stdio[1]);
  run.args[ChildStderr] = this->makeIntLiteral(loc, run.stdio[2]);
  run.args[ChildStdioGiven] = this->makeIntLiteral(loc, run.isStdioGiven ? 1 : 0);
  run.args[ChildEncoding] = this->makeIntLiteral(loc, run.encoding);
  call->args = std::move(run.args);
  call->spread.clear();
  if (isSpawn) {
    call->target = BuiltinCall{.builtin = Builtin::ChildSpawnSync};
    call->type = Type::result(types[1], failure);

    return;
  }
  bool isText = run.encoding != ChildBytes;
  const Type *text = isText ? Type::string() : Type::bytesType(Type::Bytes::Buffer);
  const Type *output = run.stdio[1] == ChildPipe ? text : Type::optional(text);
  Builtin builtin = run.isExec ? Builtin::ChildExecSync : Builtin::ChildExecFileSync;
  call->target = BuiltinCall{.builtin = builtin};
  call->type = Type::result(output, failure);
}

void LibraryChildProcessRegistry::pushDroppedCheck(Expr *value, CheckTask next) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(std::move(next));
  tasks.pushTask(DiscardChecked{});

  return tasks.pushTask(CheckExpr{.expr = value, .expected = nullptr});
}

void LibraryChildProcessRegistry::pushAbsentValue(llvm::SMLoc loc, const Type *type) {
  auto *none = this->state.ast.make<UndefinedExpr>(loc);
  none->type = Type::undefinedTy();
  this->state.walk.tasks.pushTask(Convert{.to = Type::optional(type)});

  return this->state.walk.tasks.pushChecked(none);
}

void LibraryChildProcessRegistry::pushCoercedValue(Expr *value, const Type *type) {
  this->state.walk.tasks.pushTask(Coerce{.to = Type::optional(type), .what = ""});

  return this->state.walk.tasks.pushChecked(value);
}

void LibraryChildProcessRegistry::typeStdioLiteral(Expr *given) {
  auto *number = dyn_cast<IntLitExpr>(given);
  auto *literal = dyn_cast<StringLitExpr>(given);
  if (number != nullptr) {
    number->type = Type::intTy();
  }
  if (literal != nullptr) {
    literal->type = Type::string();
  }
}

void LibraryChildProcessRegistry::walkZeroArguments(ChildRunDraft &run, const Expr *given) {
  auto *list = dyn_cast<ArrayLitExpr>(given);
  if (list == nullptr) {
    return;
  }
  for (size_t index = 0; index < list->elements.size(); ++index) {
    const ArrayLitExpr::Element &element = list->elements[index];
    if (!element.spread && this->hasZero(element.expr)) {
      auto message = ErrorTextChildRun::zeroArg(index);
      this->state.diagnostics.error(element.expr->loc(), message);
      run.isFailed = true;
    }
  }
}

bool LibraryChildProcessRegistry::hasZero(const Expr *expr) const {
  auto *literal = dyn_cast_or_null<StringLitExpr>(expr);

  return literal != nullptr && literal->value.find(u'\0') != std::u16string::npos;
}

bool LibraryChildProcessRegistry::isNegativeLiteral(const Expr *value) const {
  auto *unary = dyn_cast_or_null<UnaryExpr>(value);
  if (unary == nullptr || unary->op != UnaryOp::Neg) {
    return false;
  }

  return isa<IntLitExpr>(unary->operand) || isa<FloatLitExpr>(unary->operand);
}

Expr *LibraryChildProcessRegistry::optionOf(const ChildRunDraft &run, NodeOption option) const {
  return this->state.library.common.findNodeOption(run.call, run.optionsAt, option);
}

IntLitExpr *LibraryChildProcessRegistry::makeIntLiteral(llvm::SMLoc loc, uint64_t value) {
  auto *literal = this->state.ast.make<IntLitExpr>(loc, value);
  literal->type = Type::intTy();

  return literal;
}

const Type *LibraryChildProcessRegistry::plainOptionType(NodeOption option) const {
  const auto *string = Type::string();
  if (option == NodeOption::WORKING_DIRECTORY || option == NodeOption::ARGV0) {
    return string;
  }
  if (option == NodeOption::ENVIRONMENT) {
    return Type::dict(Type::DictKind::Record, string, string);
  }
  if (option == NodeOption::TIMEOUT || option == NodeOption::MAX_BUFFER) {
    return Type::doubleTy();
  }
  if (option == NodeOption::USER_ID || option == NodeOption::GROUP_ID) {
    return Type::intTy();
  }

  return Type::boolTy();
}

LibraryChildProcessRegistry::ResultFileName
LibraryChildProcessRegistry::checkFileName(const Expr *file, bool isExec) const {
  if (this->hasZero(file)) {
    auto message = ErrorTextChildRun::zeroFile(isExec);

    return err(LibraryChildProcessRegistryFileNameError{
        .code = LibraryChildProcessRegistryFileNameErrorCode::ZERO_IN_NAME,
        .message = message,
    });
  }
  auto *literal = dyn_cast<StringLitExpr>(file);
  if (literal != nullptr && literal->value.empty() && !isExec) {
    auto message = ErrorTextChildRun::emptyName();

    return err(LibraryChildProcessRegistryFileNameError{
        .code = LibraryChildProcessRegistryFileNameErrorCode::EMPTY_NAME,
        .message = message,
    });
  }

  return ok(file);
}

LibraryChildProcessRegistry::ResultStdioKind
LibraryChildProcessRegistry::checkStdioEntry(const ArrayLitExpr::Element &element, unsigned index) {
  if (element.spread) {
    auto message = ErrorTextChildStdio::spread();

    return err(LibraryChildProcessRegistryStdioKindError{
        .code = LibraryChildProcessRegistryStdioKindErrorCode::SPREAD_ENTRY,
        .message = message,
    });
  }
  this->typeStdioLiteral(element.expr);

  return this->checkStdioKind(element.expr, index);
}

LibraryChildProcessRegistry::ResultStdioKind
LibraryChildProcessRegistry::checkStdioKind(const Expr *given, unsigned index) const {
  if (isa<UndefinedExpr>(given)) {
    return ok(ChildPipe);
  }
  auto *number = dyn_cast<IntLitExpr>(given);
  if (number != nullptr && number->value > 0x7fffffff) {
    auto message = ErrorTextChildStdio::noFd(number->value);

    return err(LibraryChildProcessRegistryStdioKindError{
        .code = LibraryChildProcessRegistryStdioKindErrorCode::NO_DESCRIPTOR,
        .message = message,
    });
  }
  if (number != nullptr) {
    return ok(ChildInherit + unsigned(number->value));
  }
  auto *literal = dyn_cast<StringLitExpr>(given);
  if (literal == nullptr) {
    auto message = ErrorTextChildStdio::literal();

    return err(LibraryChildProcessRegistryStdioKindError{
        .code = LibraryChildProcessRegistryStdioKindErrorCode::NOT_LITERAL,
        .message = message,
    });
  }
  auto text = ToUtf8::toUtf8(literal->value);
  auto stdio = ChildStdioTable::find(text);
  if (stdio == ChildStdio::PIPE) {
    return ok(ChildPipe);
  }
  if (stdio == ChildStdio::INHERIT) {
    return ok(ChildInherit + index);
  }
  if (stdio == ChildStdio::IGNORED) {
    return ok(ChildIgnore);
  }
  if (stdio == ChildStdio::IPC) {
    auto message = ErrorTextChildStdio::ipc();

    return err(LibraryChildProcessRegistryStdioKindError{
        .code = LibraryChildProcessRegistryStdioKindErrorCode::IPC_CHANNEL,
        .message = message,
    });
  }
  auto message = ErrorTextChildStdio::unknown(text);

  return err(LibraryChildProcessRegistryStdioKindError{
      .code = LibraryChildProcessRegistryStdioKindErrorCode::UNKNOWN_KIND,
      .message = message,
  });
}

}
