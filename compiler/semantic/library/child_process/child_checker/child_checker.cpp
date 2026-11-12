#include "compiler/semantic/library/child_process/child_checker/child_checker.h"
#include "compiler/program/library/bytes/encoding_name_table/encoding_name_table.h"
#include "compiler/program/library/child_process/child_signal_table/child_signal_table.h"
#include "compiler/program/library/child_process/child_stdio_table/child_stdio_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/library/child_process/child_error_types/child_error_types.h"
#include "compiler/semantic/library/fs/fs_checker/fs_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

ChildChecker::ChildChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fsChecker = &all.fsChecker;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->bytesChecker = &all.bytesChecker;
  this->typeRelations = &all.typeRelations;
  this->argumentChecker = &all.argumentChecker;
  this->childErrorTypes = &all.childErrorTypes;
}

const Type *ChildChecker::bufferType() const {
  return Type::bytesType(Type::Bytes::Buffer);
}

bool ChildChecker::hasZero(const Expr *expr) const {
  auto *literal = llvm::dyn_cast_or_null<StringLitExpr>(expr);

  return literal != nullptr && literal->value.find(u'\0') != std::u16string::npos;
}

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

}

Expr *ChildChecker::checkChildCall(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc) {
  call->type = Type::error();
  auto function = ChildFunctionTable::find(functionName);
  if (!function.has_value()) {
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  bool isExec = function == ChildFunction::EXEC_SYNC;
  unsigned maxArgs = isExec ? 2 : 3;
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 1, maxArgs);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }

  ChildRun run{
      .call = call,
      .function = *function,
      .functionName = functionName,
      .loc = loc,
      .isExec = isExec,
      .args = std::vector<Expr *>(ChildArgCount, nullptr),
  };
  run.encoding = ChildBytes;
  run.stdio = {ChildPipe, ChildPipe, ChildPipe};
  run.isViaShell = isExec;
  this->walkFileArgument(run);
  auto optionsAt = this->walkArgumentList(run);
  if (!optionsAt.has_value()) {
    return call;
  }
  if (!this->fsChecker->readNodeOptions(call, *optionsAt, functionName, kChildOptions,
                                        run.options)) {
    return call;
  }
  this->walkEncodingOption(run);
  this->walkStdioOption(run);
  this->walkOptionLimits(run);
  this->fillPlainOptions(run);
  this->walkInputOption(run);
  this->walkKillSignalOption(run);
  this->walkShellOption(run);
  if (run.isFailed) {
    return call;
  }

  return this->finishChildCall(run);
}

Expr *ChildChecker::absentValue(llvm::SMLoc loc, const Type *type) {
  auto *none = this->state.getAst().make<UndefinedExpr>(loc);
  none->type = Type::undefinedTy();

  return this->typeRelations->convert(none, Type::optional(type));
}

IntLitExpr *ChildChecker::makeIntLiteral(llvm::SMLoc loc, uint64_t value) {
  auto *literal = this->state.getAst().make<IntLitExpr>(loc, value);
  literal->type = Type::intTy();

  return literal;
}

void ChildChecker::walkFileArgument(ChildRun &run) {
  CallExpr *call = run.call;
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  run.args[ChildFile] = call->args[0];
  if (this->hasZero(call->args[0])) {
    auto message = ErrorTextChildRun::zeroFile(run.isExec);
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    run.isFailed = true;
    return;
  }
  auto *literal = dyn_cast<StringLitExpr>(call->args[0]);
  if (literal != nullptr && literal->value.empty() && !run.isExec) {
    auto message = ErrorTextChildRun::emptyName();
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    run.isFailed = true;
  }
}

std::optional<size_t> ChildChecker::walkArgumentList(ChildRun &run) {
  CallExpr *call = run.call;
  bool isListGiven = !run.isExec && call->args.size() > 1 && !isa<ObjectLitExpr>(call->args[1]);
  if (!run.isExec && !isListGiven && call->args.size() > 2) {
    auto message = ErrorTextChildRun::optionsAt(run.functionName);
    this->state.getDiagnostics().error(call->args[2]->loc(), message);
    return std::nullopt;
  }
  if (isListGiven && !isa<UndefinedExpr>(call->args[1])) {
    this->walkZeroArguments(run, call->args[1]);
    call->args[1] = this->argumentChecker->checkArg(call->args[1], Type::array(Type::string()));
    run.args[ChildArgs] = call->args[1];
  }
  if (run.args[ChildArgs] == nullptr) {
    auto *none = this->state.getAst().make<ArrayLitExpr>(run.loc);
    none->type = Type::array(Type::string());
    run.args[ChildArgs] = none;
  }

  return isListGiven ? 2 : 1;
}

void ChildChecker::walkZeroArguments(ChildRun &run, const Expr *given) {
  auto *list = dyn_cast<ArrayLitExpr>(given);
  if (list == nullptr) {
    return;
  }
  for (size_t index = 0; index < list->elements.size(); ++index) {
    const ArrayLitExpr::Element &element = list->elements[index];
    if (!element.spread && this->hasZero(element.expr)) {
      auto message = ErrorTextChildRun::zeroArg(index);
      this->state.getDiagnostics().error(element.expr->loc(), message);
      run.isFailed = true;
    }
  }
}

void ChildChecker::walkEncodingOption(ChildRun &run) {
  Expr *value = run.options.lookup(NodeOption::ENCODING);
  if (value == nullptr || isa<UndefinedExpr>(value)) {
    return;
  }
  auto *literal = dyn_cast<StringLitExpr>(value);
  if (literal == nullptr) {
    auto message = ErrorTextChildOption::encoding(run.functionName);
    this->state.getDiagnostics().error(value->loc(), message);
    this->exprChecker->checkExpr(value);
    run.isFailed = true;
    return;
  }
  auto written = ToUtf8::toUtf8(literal->value);
  if (EncodingNameTable::isBufferMarker(written)) {
    return;
  }
  literal->type = Type::string();
  auto *number = this->bytesChecker->checkBufferEncoding(literal);
  if (number->type->isError()) {
    run.isFailed = true;
    return;
  }
  run.encoding = 1 + unsigned(cast<IntLitExpr>(number)->value);
}

void ChildChecker::walkStdioOption(ChildRun &run) {
  Expr *value = run.options.lookup(NodeOption::STDIO);
  if (value == nullptr || isa<UndefinedExpr>(value)) {
    return;
  }
  run.isStdioGiven = true;
  auto *list = dyn_cast<ArrayLitExpr>(value);
  if (list != nullptr) {
    return this->walkStdioList(run, list);
  }
  if (isa<StringLitExpr>(value)) {
    return this->walkStdioText(run, value);
  }
  auto message = ErrorTextChildOption::stdio(run.functionName);
  this->state.getDiagnostics().error(value->loc(), message);
  this->exprChecker->checkExpr(value);
  run.isFailed = true;
}

void ChildChecker::walkStdioList(ChildRun &run, const ArrayLitExpr *list) {
  if (list->elements.size() > 3) {
    auto message = ErrorTextChildStdio::three();
    this->state.getDiagnostics().error(list->elements[3].expr->loc(), message);
    run.isFailed = true;
  }
  for (size_t index = 0; index < list->elements.size() && index < 3; ++index) {
    const ArrayLitExpr::Element &element = list->elements[index];
    std::optional<unsigned> kind =
        element.spread ? std::nullopt : this->stdioKind(element.expr, unsigned(index));
    if (element.spread) {
      auto message = ErrorTextChildStdio::spread();
      this->state.getDiagnostics().error(element.expr->loc(), message);
    }
    if (kind.has_value()) {
      run.stdio[index] = *kind;
    } else {
      run.isFailed = true;
    }
  }
}

void ChildChecker::walkStdioText(ChildRun &run, Expr *value) {
  for (unsigned index = 0; index < 3; ++index) {
    auto kind = this->stdioKind(value, index);
    if (!kind.has_value()) {
      run.isFailed = true;
      return;
    }
    run.stdio[index] = *kind;
  }
}

std::optional<unsigned> ChildChecker::stdioKind(Expr *given, unsigned index) {
  if (isa<UndefinedExpr>(given)) {
    return ChildPipe;
  }
  auto *number = dyn_cast<IntLitExpr>(given);
  if (number != nullptr) {
    number->type = Type::intTy();
    if (number->value > 0x7fffffff) {
      auto message = ErrorTextChildStdio::noFd(number->value);
      this->state.getDiagnostics().error(given->loc(), message);
      return std::nullopt;
    }
    return ChildInherit + unsigned(number->value);
  }
  auto *literal = dyn_cast<StringLitExpr>(given);
  if (literal == nullptr) {
    auto message = ErrorTextChildStdio::literal();
    this->state.getDiagnostics().error(given->loc(), message);
    this->exprChecker->checkExpr(given);
    return std::nullopt;
  }
  literal->type = Type::string();
  auto text = ToUtf8::toUtf8(literal->value);
  auto stdio = ChildStdioTable::find(text);
  if (stdio == ChildStdio::PIPE) {
    return ChildPipe;
  }
  if (stdio == ChildStdio::INHERIT) {
    return ChildInherit + index;
  }
  if (stdio == ChildStdio::IGNORED) {
    return ChildIgnore;
  }
  std::string message =
      stdio == ChildStdio::IPC ? ErrorTextChildStdio::ipc() : ErrorTextChildStdio::unknown(text);
  this->state.getDiagnostics().error(given->loc(), message);

  return std::nullopt;
}

void ChildChecker::walkOptionLimits(ChildRun &run) {
  NodeOptions &options = run.options;
  Expr *directory = options.lookup(NodeOption::WORKING_DIRECTORY);
  if (this->hasZero(directory)) {
    auto message = ErrorTextChildRun::zeroCwd();
    this->state.getDiagnostics().error(directory->loc(), message);
    run.isFailed = true;
  }
  for (NodeOption limit : {NodeOption::TIMEOUT, NodeOption::MAX_BUFFER}) {
    Expr *value = options.lookup(limit);
    auto *unary = value != nullptr ? dyn_cast<UnaryExpr>(value) : nullptr;
    bool isNegative = unary != nullptr && unary->op == UnaryOp::Neg &&
                      (isa<IntLitExpr>(unary->operand) || isa<FloatLitExpr>(unary->operand));
    if (isNegative) {
      auto name = NodeOptionTable::getName(limit);
      auto message = ErrorTextChildRun::negative(name);
      this->state.getDiagnostics().error(value->loc(), message);
      run.isFailed = true;
    }
  }
  bool isWindows = NodeCheckHelpers::isTargetWindows();
  if (isWindows) {
    for (NodeOption id : {NodeOption::USER_ID, NodeOption::GROUP_ID}) {
      Expr *value = options.lookup(id);
      if (value != nullptr && !isa<UndefinedExpr>(value)) {
        auto name = NodeOptionTable::getName(id);
        auto message = ErrorTextChildRun::unixId(name);
        this->state.getDiagnostics().error(value->loc(), message);
        run.isFailed = true;
      }
    }
  }
  auto *signal = dyn_cast_or_null<StringLitExpr>(options.lookup(NodeOption::KILL_SIGNAL));
  if (signal != nullptr) {
    auto written = ToUtf8::toUtf8(signal->value);
    if (!ChildSignalTable::isKnown(written, isWindows)) {
      auto message = ErrorTextChildRun::noSignal(written);
      this->state.getDiagnostics().error(signal->loc(), message);
      run.isFailed = true;
    }
  }
}

Expr *ChildChecker::optionValue(ChildRun &run, NodeOption option, const Type *type) {
  Expr *value = run.options.lookup(option);
  if (value != nullptr) {
    return this->argumentChecker->checkArg(value, Type::optional(type));
  }

  return this->absentValue(run.loc, type);
}

void ChildChecker::fillPlainOptions(ChildRun &run) {
  const auto *string = Type::string();
  const auto *environment = Type::dict(Type::DictKind::Record, string, string);
  std::vector<Expr *> &args = run.args;
  args[ChildCwd] = this->optionValue(run, NodeOption::WORKING_DIRECTORY, string);
  args[ChildEnv] = this->optionValue(run, NodeOption::ENVIRONMENT, environment);
  args[ChildTimeout] = this->optionValue(run, NodeOption::TIMEOUT, Type::doubleTy());
  args[ChildMaxBuffer] = this->optionValue(run, NodeOption::MAX_BUFFER, Type::doubleTy());
  args[ChildWindowsHide] = this->optionValue(run, NodeOption::WINDOWS_HIDE, Type::boolTy());
  args[ChildVerbatim] =
      this->optionValue(run, NodeOption::WINDOWS_VERBATIM_ARGUMENTS, Type::boolTy());
  args[ChildDetached] = this->optionValue(run, NodeOption::DETACHED, Type::boolTy());
  args[ChildArgv0] = this->optionValue(run, NodeOption::ARGV0, string);
  args[ChildUid] = this->optionValue(run, NodeOption::USER_ID, Type::intTy());
  args[ChildGid] = this->optionValue(run, NodeOption::GROUP_ID, Type::intTy());
}

void ChildChecker::walkInputOption(ChildRun &run) {
  const auto *string = Type::string();
  Expr *value = run.options.lookup(NodeOption::INPUT);
  if (value == nullptr) {
    run.args[ChildInput] = this->absentValue(run.loc, string);
    return;
  }
  auto *input = this->exprChecker->checkExpr(value, string);
  if (input->type->nonOptional()->isStringLiteral()) {
    input = this->typeRelations->coerce(input, string);
  }
  const Type *type = input->type->nonOptional();
  if (type->isError()) {
    run.isFailed = true;
    return;
  }
  if (!type->isString() && !type->isByteView()) {
    auto message = ErrorTextChildOption::input(run.functionName, input->type->name());
    this->state.getDiagnostics().error(input->loc(), message);
    run.isFailed = true;
    return;
  }
  run.args[ChildInput] = this->typeRelations->coerce(input, Type::optional(type));
  Expr *encoding = run.options.lookup(NodeOption::ENCODING);
  bool isBufferText = type->isString() && run.encoding == ChildBytes && encoding != nullptr &&
                      !isa<UndefinedExpr>(encoding);
  if (isBufferText) {
    auto message = ErrorTextChildOption::bufferText();
    this->state.getDiagnostics().error(input->loc(), message);
    run.isFailed = true;
  }
}

void ChildChecker::walkKillSignalOption(ChildRun &run) {
  const auto *string = Type::string();
  Expr *value = run.options.lookup(NodeOption::KILL_SIGNAL);
  if (value == nullptr) {
    run.args[ChildKillSignal] = this->absentValue(run.loc, string);
    return;
  }
  auto *signal = this->exprChecker->checkExpr(value, string);
  const Type *type = signal->type->nonOptional();
  if (type->isStringLiteral() || type->isString()) {
    run.args[ChildKillSignal] = this->typeRelations->coerce(signal, Type::optional(string));
    return;
  }
  bool isNumber = type->isInt() || (type->isLiteral() && type->literalBase()->isInt());
  if (isNumber) {
    run.args[ChildKillSignal] = this->typeRelations->coerce(signal, Type::optional(Type::intTy()));
    return;
  }
  if (!type->isError()) {
    auto message = ErrorTextChildOption::killSignal(signal->type->name());
    this->state.getDiagnostics().error(signal->loc(), message);
  }
  run.isFailed = true;
}

void ChildChecker::walkShellOption(ChildRun &run) {
  const auto *string = Type::string();
  Expr *value = run.options.lookup(NodeOption::SHELL);
  if (value == nullptr) {
    run.args[ChildShell] = this->absentValue(run.loc, string);
    return;
  }
  auto *written = dyn_cast<BoolLitExpr>(value);
  if (written != nullptr) {
    run.isViaShell = run.isViaShell || written->value;
  }
  auto *shell = this->exprChecker->checkExpr(value, string);
  const Type *type = shell->type->nonOptional();
  bool isBool = type->isBool() || (type->isLiteral() && type->literalBase()->isBool());
  if (type->isStringLiteral() || type->isString()) {
    run.isViaShell = true;
    run.args[ChildShell] = this->typeRelations->coerce(shell, Type::optional(string));
    return;
  }
  if (isBool && run.isExec) {
    return this->walkExecShell(run, shell);
  }
  if (isBool && !isa<BoolLitExpr>(value)) {
    auto message = ErrorTextChildOption::shellFlag(run.functionName);
    this->state.getDiagnostics().error(shell->loc(), message);
    run.isFailed = true;
    return;
  }
  if (isBool) {
    run.args[ChildShell] = this->typeRelations->coerce(shell, Type::optional(Type::boolTy()));
    return;
  }
  if (!type->isError()) {
    auto message = ErrorTextChildOption::shell(shell->type->name());
    this->state.getDiagnostics().error(shell->loc(), message);
  }
  run.isFailed = true;
}

void ChildChecker::walkExecShell(ChildRun &run, Expr *shell) {
  auto *literal = dyn_cast<BoolLitExpr>(shell);
  if (literal != nullptr && literal->value) {
    run.args[ChildShell] = this->absentValue(run.loc, Type::string());
    return;
  }
  auto message = ErrorTextChildOption::execShell();
  this->state.getDiagnostics().error(shell->loc(), message);
  run.isFailed = true;
}

Expr *ChildChecker::finishChildCall(ChildRun &run) {
  CallExpr *call = run.call;
  llvm::SMLoc loc = run.loc;
  run.args[ChildStdin] = this->makeIntLiteral(loc, run.stdio[0]);
  run.args[ChildStdout] = this->makeIntLiteral(loc, run.stdio[1]);
  run.args[ChildStderr] = this->makeIntLiteral(loc, run.stdio[2]);
  run.args[ChildStdioGiven] = this->makeIntLiteral(loc, run.isStdioGiven ? 1 : 0);
  run.args[ChildEncoding] = this->makeIntLiteral(loc, run.encoding);
  call->args = std::move(run.args);
  call->spread.clear();

  bool isTextOutput = run.encoding != ChildBytes;
  const Type *text = isTextOutput ? Type::string() : this->bufferType();
  const auto *failure = this->childErrorTypes->errorOf(run.function, isTextOutput, run.isViaShell);
  if (run.function == ChildFunction::SPAWN_SYNC) {
    const auto *spawned = this->childErrorTypes->spawnResult(isTextOutput);
    call->target = BuiltinCall{.builtin = Builtin::ChildSpawnSync};
    call->type = Type::result(spawned, failure);
    return call;
  }
  const Type *output = run.stdio[1] == ChildPipe ? text : Type::optional(text);
  call->target =
      BuiltinCall{.builtin = run.isExec ? Builtin::ChildExecSync : Builtin::ChildExecFileSync};
  call->type = Type::result(output, failure);

  return call;
}

}
