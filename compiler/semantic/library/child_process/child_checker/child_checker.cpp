#include "compiler/semantic/library/child_process/child_checker/child_checker.h"
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
  this->fsChecker = &all.getFsChecker();
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
  this->bytesChecker = &all.getBytesChecker();
  this->typeRelations = &all.getTypeRelations();
  this->argumentChecker = &all.getArgumentChecker();
  this->childErrorTypes = &all.getChildErrorTypes();
}

const Type *ChildChecker::bufferType() {
  return Type::bytesType(Type::Bytes::Buffer);
}

bool ChildChecker::knownSignal(llvm::StringRef name) {
  static const llvm::StringLiteral windows[] = {"SIGHUP",  "SIGINT",   "SIGQUIT", "SIGILL",
                                                "SIGABRT", "SIGFPE",   "SIGKILL", "SIGSEGV",
                                                "SIGTERM", "SIGBREAK", "SIGWINCH"};
  static const llvm::StringLiteral posix[] = {
      "SIGHUP",   "SIGINT",  "SIGQUIT", "SIGILL",    "SIGTRAP", "SIGABRT",   "SIGIOT",
      "SIGBUS",   "SIGFPE",  "SIGKILL", "SIGUSR1",   "SIGSEGV", "SIGUSR2",   "SIGPIPE",
      "SIGALRM",  "SIGTERM", "SIGCHLD", "SIGSTKFLT", "SIGCONT", "SIGSTOP",   "SIGTSTP",
      "SIGTTIN",  "SIGTTOU", "SIGURG",  "SIGXCPU",   "SIGXFSZ", "SIGVTALRM", "SIGPROF",
      "SIGWINCH", "SIGIO",   "SIGPOLL", "SIGPWR",    "SIGSYS"};
  if (NodeCheckHelpers::targetIsWindows()) {
    return llvm::is_contained(windows, name);
  }

  return llvm::is_contained(posix, name);
}

bool ChildChecker::hasZero(const Expr *expr) {
  auto *lit = llvm::dyn_cast_or_null<StringLitExpr>(expr);

  return lit && lit->value.find(u'\0') != std::u16string::npos;
}

namespace {

const llvm::StringRef kChildOptions[] = {"cwd",
                                         "env",
                                         "input",
                                         "stdio",
                                         "encoding",
                                         "timeout",
                                         "killSignal",
                                         "maxBuffer",
                                         "shell",
                                         "windowsHide",
                                         "windowsVerbatimArguments",
                                         "detached",
                                         "argv0",
                                         "uid",
                                         "gid"};

}

Expr *ChildChecker::checkChildCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  call->type = Type::error();
  std::optional<ChildFunction> function = ChildFunctionTable::find(fn);
  if (!function.has_value()) {
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  bool exec = function == ChildFunction::EXEC_SYNC;
  bool rejected = this->callChecker->rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->argumentChecker->checkArity(call, 1, exec ? 2 : 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }

  ChildRun run{
      .call = call,
      .function = *function,
      .fn = fn,
      .loc = loc,
      .exec = exec,
      .args = std::vector<Expr *>(ChildArgCount, nullptr),
  };
  run.encoding = ChildBytes;
  run.stdio = {ChildPipe, ChildPipe, ChildPipe};
  run.viaShell = exec;
  this->walkFileArgument(run);
  std::optional<size_t> optionsAt = this->walkArgumentList(run);
  if (!optionsAt.has_value()) {
    return call;
  }
  if (!this->fsChecker->nodeOptions(call, *optionsAt, fn, kChildOptions, run.options)) {
    return call;
  }
  this->walkEncodingOption(run);
  this->walkStdioOption(run);
  this->walkOptionLimits(run);
  this->fillPlainOptions(run);
  this->walkInputOption(run);
  this->walkKillSignalOption(run);
  this->walkShellOption(run);
  if (run.failed) {
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
  if (ChildChecker::hasZero(call->args[0])) {
    std::string message = ErrorTextChildRun::zeroFile(run.exec);
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    run.failed = true;
    return;
  }
  auto *literal = dyn_cast<StringLitExpr>(call->args[0]);
  if (literal != nullptr && literal->value.empty() && !run.exec) {
    std::string message = ErrorTextChildRun::emptyName();
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    run.failed = true;
  }
}

std::optional<size_t> ChildChecker::walkArgumentList(ChildRun &run) {
  CallExpr *call = run.call;
  bool listGiven = !run.exec && call->args.size() > 1 && !isa<ObjectLitExpr>(call->args[1]);
  if (!run.exec && !listGiven && call->args.size() > 2) {
    std::string message = ErrorTextChildRun::optionsAt(run.fn);
    this->state.getDiagnostics().error(call->args[2]->loc(), message);
    return std::nullopt;
  }
  if (listGiven && !isa<UndefinedExpr>(call->args[1])) {
    this->walkZeroArguments(run, call->args[1]);
    call->args[1] = this->argumentChecker->checkArg(call->args[1], Type::array(Type::string()));
    run.args[ChildArgs] = call->args[1];
  }
  if (run.args[ChildArgs] == nullptr) {
    auto *none = this->state.getAst().make<ArrayLitExpr>(run.loc);
    none->type = Type::array(Type::string());
    run.args[ChildArgs] = none;
  }

  return listGiven ? 2 : 1;
}

void ChildChecker::walkZeroArguments(ChildRun &run, const Expr *given) {
  auto *list = dyn_cast<ArrayLitExpr>(given);
  if (list == nullptr) {
    return;
  }
  for (size_t i = 0; i < list->elements.size(); ++i) {
    const ArrayLitExpr::Element &element = list->elements[i];
    if (!element.spread && ChildChecker::hasZero(element.expr)) {
      std::string message = ErrorTextChildRun::zeroArg(i);
      this->state.getDiagnostics().error(element.expr->loc(), message);
      run.failed = true;
    }
  }
}

void ChildChecker::walkEncodingOption(ChildRun &run) {
  Expr *value = run.options.lookup("encoding");
  if (value == nullptr || isa<UndefinedExpr>(value)) {
    return;
  }
  auto *literal = dyn_cast<StringLitExpr>(value);
  if (literal == nullptr) {
    std::string message = ErrorTextChildOption::encoding(run.fn);
    this->state.getDiagnostics().error(value->loc(), message);
    this->exprChecker->checkExpr(value);
    run.failed = true;
    return;
  }
  if (literal->value == u"buffer") {
    return;
  }
  literal->type = Type::string();
  Expr *number = this->bytesChecker->checkBufferEncoding(literal);
  if (number->type->isError()) {
    run.failed = true;
    return;
  }
  run.encoding = 1 + unsigned(cast<IntLitExpr>(number)->value);
}

void ChildChecker::walkStdioOption(ChildRun &run) {
  Expr *value = run.options.lookup("stdio");
  if (value == nullptr || isa<UndefinedExpr>(value)) {
    return;
  }
  run.stdioGiven = true;
  auto *list = dyn_cast<ArrayLitExpr>(value);
  if (list != nullptr) {
    return this->walkStdioList(run, list);
  }
  if (isa<StringLitExpr>(value)) {
    return this->walkStdioText(run, value);
  }
  std::string message = ErrorTextChildOption::stdio(run.fn);
  this->state.getDiagnostics().error(value->loc(), message);
  this->exprChecker->checkExpr(value);
  run.failed = true;
}

void ChildChecker::walkStdioList(ChildRun &run, const ArrayLitExpr *list) {
  if (list->elements.size() > 3) {
    std::string message = ErrorTextChildStdio::three();
    this->state.getDiagnostics().error(list->elements[3].expr->loc(), message);
    run.failed = true;
  }
  for (size_t i = 0; i < list->elements.size() && i < 3; ++i) {
    const ArrayLitExpr::Element &element = list->elements[i];
    std::optional<unsigned> kind =
        element.spread ? std::nullopt : this->stdioKind(element.expr, unsigned(i));
    if (element.spread) {
      std::string message = ErrorTextChildStdio::spread();
      this->state.getDiagnostics().error(element.expr->loc(), message);
    }
    if (kind.has_value()) {
      run.stdio[i] = *kind;
    } else {
      run.failed = true;
    }
  }
}

void ChildChecker::walkStdioText(ChildRun &run, Expr *value) {
  for (unsigned i = 0; i < 3; ++i) {
    std::optional<unsigned> kind = this->stdioKind(value, i);
    if (!kind.has_value()) {
      run.failed = true;
      return;
    }
    run.stdio[i] = *kind;
  }
}

std::optional<unsigned> ChildChecker::stdioKind(Expr *given, unsigned i) {
  if (isa<UndefinedExpr>(given)) {
    return ChildPipe;
  }
  auto *number = dyn_cast<IntLitExpr>(given);
  if (number != nullptr) {
    number->type = Type::intTy();
    if (number->value > 0x7fffffff) {
      std::string message = ErrorTextChildStdio::noFd(number->value);
      this->state.getDiagnostics().error(given->loc(), message);
      return std::nullopt;
    }
    return ChildInherit + unsigned(number->value);
  }
  auto *literal = dyn_cast<StringLitExpr>(given);
  if (literal == nullptr) {
    std::string message = ErrorTextChildStdio::literal();
    this->state.getDiagnostics().error(given->loc(), message);
    this->exprChecker->checkExpr(given);
    return std::nullopt;
  }
  literal->type = Type::string();
  std::string text = ToUtf8::toUtf8(literal->value);
  if (text == "pipe" || text == "overlapped") {
    return ChildPipe;
  }
  if (text == "inherit") {
    return ChildInherit + i;
  }
  if (text == "ignore") {
    return ChildIgnore;
  }
  std::string message =
      text == "ipc" ? ErrorTextChildStdio::ipc() : ErrorTextChildStdio::unknown(text);
  this->state.getDiagnostics().error(given->loc(), message);

  return std::nullopt;
}

void ChildChecker::walkOptionLimits(ChildRun &run) {
  llvm::StringMap<Expr *> &options = run.options;
  if (ChildChecker::hasZero(options.lookup("cwd"))) {
    std::string message = ErrorTextChildRun::zeroCwd();
    this->state.getDiagnostics().error(options.lookup("cwd")->loc(), message);
    run.failed = true;
  }
  for (llvm::StringRef limit : {"timeout", "maxBuffer"}) {
    Expr *value = options.lookup(limit);
    auto *unary = value != nullptr ? dyn_cast<UnaryExpr>(value) : nullptr;
    bool negative = unary != nullptr && unary->op == UnaryOp::Neg &&
                    (isa<IntLitExpr>(unary->operand) || isa<FloatLitExpr>(unary->operand));
    if (negative) {
      std::string message = ErrorTextChildRun::negative(limit);
      this->state.getDiagnostics().error(value->loc(), message);
      run.failed = true;
    }
  }
  if (NodeCheckHelpers::targetIsWindows()) {
    for (llvm::StringRef id : {"uid", "gid"}) {
      Expr *value = options.lookup(id);
      if (value != nullptr && !isa<UndefinedExpr>(value)) {
        std::string message = ErrorTextChildRun::unixId(id);
        this->state.getDiagnostics().error(value->loc(), message);
        run.failed = true;
      }
    }
  }
  auto *signal = dyn_cast_or_null<StringLitExpr>(options.lookup("killSignal"));
  if (signal != nullptr) {
    std::string written = ToUtf8::toUtf8(signal->value);
    if (!ChildChecker::knownSignal(written)) {
      std::string message = ErrorTextChildRun::noSignal(written);
      this->state.getDiagnostics().error(signal->loc(), message);
      run.failed = true;
    }
  }
}

Expr *ChildChecker::optionValue(ChildRun &run, llvm::StringRef name, const Type *type) {
  Expr *value = run.options.lookup(name);
  if (value != nullptr) {
    return this->argumentChecker->checkArg(value, Type::optional(type));
  }

  return this->absentValue(run.loc, type);
}

void ChildChecker::fillPlainOptions(ChildRun &run) {
  const Type *string = Type::string();
  std::vector<Expr *> &args = run.args;
  args[ChildCwd] = this->optionValue(run, "cwd", string);
  args[ChildEnv] =
      this->optionValue(run, "env", Type::dict(Type::DictKind::Record, string, string));
  args[ChildTimeout] = this->optionValue(run, "timeout", Type::doubleTy());
  args[ChildMaxBuffer] = this->optionValue(run, "maxBuffer", Type::doubleTy());
  args[ChildWindowsHide] = this->optionValue(run, "windowsHide", Type::boolTy());
  args[ChildVerbatim] = this->optionValue(run, "windowsVerbatimArguments", Type::boolTy());
  args[ChildDetached] = this->optionValue(run, "detached", Type::boolTy());
  args[ChildArgv0] = this->optionValue(run, "argv0", string);
  args[ChildUid] = this->optionValue(run, "uid", Type::intTy());
  args[ChildGid] = this->optionValue(run, "gid", Type::intTy());
}

void ChildChecker::walkInputOption(ChildRun &run) {
  const Type *string = Type::string();
  Expr *value = run.options.lookup("input");
  if (value == nullptr) {
    run.args[ChildInput] = this->absentValue(run.loc, string);
    return;
  }
  Expr *input = this->exprChecker->checkExpr(value, string);
  if (input->type->nonOptional()->isStringLiteral()) {
    input = this->typeRelations->coerce(input, string);
  }
  const Type *type = input->type->nonOptional();
  if (type->isError()) {
    run.failed = true;
    return;
  }
  if (!type->isString() && !type->isByteView()) {
    std::string message = ErrorTextChildOption::input(run.fn, input->type->name());
    this->state.getDiagnostics().error(input->loc(), message);
    run.failed = true;
    return;
  }
  run.args[ChildInput] = this->typeRelations->coerce(input, Type::optional(type));
  Expr *encoding = run.options.lookup("encoding");
  bool bufferText = type->isString() && run.encoding == ChildBytes && encoding != nullptr &&
                    !isa<UndefinedExpr>(encoding);
  if (bufferText) {
    std::string message = ErrorTextChildOption::bufferText();
    this->state.getDiagnostics().error(input->loc(), message);
    run.failed = true;
  }
}

void ChildChecker::walkKillSignalOption(ChildRun &run) {
  const Type *string = Type::string();
  Expr *value = run.options.lookup("killSignal");
  if (value == nullptr) {
    run.args[ChildKillSignal] = this->absentValue(run.loc, string);
    return;
  }
  Expr *signal = this->exprChecker->checkExpr(value, string);
  const Type *type = signal->type->nonOptional();
  if (type->isStringLiteral() || type->isString()) {
    run.args[ChildKillSignal] = this->typeRelations->coerce(signal, Type::optional(string));
    return;
  }
  bool number = type->isInt() || (type->isLiteral() && type->literalBase()->isInt());
  if (number) {
    run.args[ChildKillSignal] = this->typeRelations->coerce(signal, Type::optional(Type::intTy()));
    return;
  }
  if (!type->isError()) {
    std::string message = ErrorTextChildOption::killSignal(signal->type->name());
    this->state.getDiagnostics().error(signal->loc(), message);
  }
  run.failed = true;
}

void ChildChecker::walkShellOption(ChildRun &run) {
  const Type *string = Type::string();
  Expr *value = run.options.lookup("shell");
  if (value == nullptr) {
    run.args[ChildShell] = this->absentValue(run.loc, string);
    return;
  }
  auto *written = dyn_cast<BoolLitExpr>(value);
  if (written != nullptr) {
    run.viaShell = run.viaShell || written->value;
  }
  Expr *shell = this->exprChecker->checkExpr(value, string);
  const Type *type = shell->type->nonOptional();
  bool isBool = type->isBool() || (type->isLiteral() && type->literalBase()->isBool());
  if (type->isStringLiteral() || type->isString()) {
    run.viaShell = true;
    run.args[ChildShell] = this->typeRelations->coerce(shell, Type::optional(string));
    return;
  }
  if (isBool && run.exec) {
    return this->walkExecShell(run, shell);
  }
  if (isBool && !isa<BoolLitExpr>(value)) {
    std::string message = ErrorTextChildOption::shellFlag(run.fn);
    this->state.getDiagnostics().error(shell->loc(), message);
    run.failed = true;
    return;
  }
  if (isBool) {
    run.args[ChildShell] = this->typeRelations->coerce(shell, Type::optional(Type::boolTy()));
    return;
  }
  if (!type->isError()) {
    std::string message = ErrorTextChildOption::shell(shell->type->name());
    this->state.getDiagnostics().error(shell->loc(), message);
  }
  run.failed = true;
}

void ChildChecker::walkExecShell(ChildRun &run, Expr *shell) {
  auto *literal = dyn_cast<BoolLitExpr>(shell);
  if (literal != nullptr && literal->value) {
    run.args[ChildShell] = this->absentValue(run.loc, Type::string());
    return;
  }
  std::string message = ErrorTextChildOption::execShell();
  this->state.getDiagnostics().error(shell->loc(), message);
  run.failed = true;
}

Expr *ChildChecker::finishChildCall(ChildRun &run) {
  CallExpr *call = run.call;
  llvm::SMLoc loc = run.loc;
  run.args[ChildStdin] = this->makeIntLiteral(loc, run.stdio[0]);
  run.args[ChildStdout] = this->makeIntLiteral(loc, run.stdio[1]);
  run.args[ChildStderr] = this->makeIntLiteral(loc, run.stdio[2]);
  run.args[ChildStdioGiven] = this->makeIntLiteral(loc, run.stdioGiven ? 1 : 0);
  run.args[ChildEncoding] = this->makeIntLiteral(loc, run.encoding);
  call->args = std::move(run.args);
  call->spread.clear();

  bool textOutput = run.encoding != ChildBytes;
  const Type *text = textOutput ? Type::string() : ChildChecker::bufferType();
  const Type *failure = this->childErrorTypes->errorOf(run.fn, textOutput, run.viaShell);
  if (run.function == ChildFunction::SPAWN_SYNC) {
    const Type *spawned = this->childErrorTypes->spawnResult(textOutput);
    call->target = BuiltinCall{.builtin = Builtin::ChildSpawnSync};
    call->type = Type::result(spawned, failure);
    return call;
  }
  const Type *output = run.stdio[1] == ChildPipe ? text : Type::optional(text);
  call->target =
      BuiltinCall{.builtin = run.exec ? Builtin::ChildExecSync : Builtin::ChildExecFileSync};
  call->type = Type::result(output, failure);

  return call;
}
}
