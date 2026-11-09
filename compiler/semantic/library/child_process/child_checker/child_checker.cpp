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

Expr *ChildChecker::checkChildCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  call->type = Type::error();
  if (fn != "spawnSync" && fn != "execFileSync" && fn != "execSync") {
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  bool exec = fn == "execSync";
  bool rejected = this->callChecker->rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->argumentChecker->checkArity(call, 1, exec ? 2 : 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  const Type *string = Type::string();
  auto intLit = [&](uint64_t value) {
    auto *lit = this->state.getAst().make<IntLitExpr>(loc, value);
    lit->type = Type::intTy();

    return lit;
  };
  auto absent = [&](const Type *type) {
    auto *none = this->state.getAst().make<UndefinedExpr>(loc);
    none->type = Type::undefinedTy();

    return this->typeRelations->convert(none, Type::optional(type));
  };

  std::vector<Expr *> args(ChildArgCount, nullptr);
  args[ChildFile] = call->args[0] = this->argumentChecker->checkArg(call->args[0], string);
  bool failed = false;
  if (ChildChecker::hasZero(call->args[0])) {
    std::string message = ErrorTextChildRun::zeroFile(exec);
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    failed = true;
  } else if (auto *lit = dyn_cast<StringLitExpr>(call->args[0]);
             lit && lit->value.empty() && !exec) {
    std::string message = ErrorTextChildRun::emptyName();
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    failed = true;
  }
  size_t optionsAt = 1;
  if (!exec && call->args.size() > 1 && !isa<ObjectLitExpr>(call->args[1])) {
    if (!isa<UndefinedExpr>(call->args[1])) {
      if (auto *list = dyn_cast<ArrayLitExpr>(call->args[1])) {
        for (size_t i = 0; i < list->elements.size(); ++i) {
          if (!list->elements[i].spread && ChildChecker::hasZero(list->elements[i].expr)) {
            std::string message = ErrorTextChildRun::zeroArg(i);
            this->state.getDiagnostics().error(list->elements[i].expr->loc(), message);
            failed = true;
          }
        }
      }
      args[ChildArgs] = call->args[1] =
          this->argumentChecker->checkArg(call->args[1], Type::array(string));
    }
    optionsAt = 2;
  } else if (!exec && call->args.size() > 2) {
    std::string message = ErrorTextChildRun::optionsAt(fn);
    this->state.getDiagnostics().error(call->args[2]->loc(), message);
    return call;
  }
  if (!args[ChildArgs]) {
    auto *none = this->state.getAst().make<ArrayLitExpr>(loc);
    none->type = Type::array(string);
    args[ChildArgs] = none;
  }

  llvm::StringMap<Expr *> options;
  static const llvm::StringRef allowed[] = {"cwd",
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
  if (!this->fsChecker->nodeOptions(call, optionsAt, fn, allowed, options)) {
    return call;
  }

  unsigned encoding = ChildBytes;
  if (Expr *value = options.lookup("encoding"); value && !isa<UndefinedExpr>(value)) {
    auto *lit = dyn_cast<StringLitExpr>(value);
    if (!lit) {
      std::string message = ErrorTextChildOption::encoding(fn);
      this->state.getDiagnostics().error(value->loc(), message);
      this->exprChecker->checkExpr(value);
      failed = true;
    } else if (lit->value != u"buffer") {
      lit->type = string;
      Expr *number = this->bytesChecker->checkBufferEncoding(lit);
      if (number->type->isError()) {
        failed = true;
      } else {
        encoding = 1 + unsigned(cast<IntLitExpr>(number)->value);
      }
    }
  }
  unsigned stdio[3] = {ChildPipe, ChildPipe, ChildPipe};
  bool stdioGiven = false;
  if (Expr *value = options.lookup("stdio"); value && !isa<UndefinedExpr>(value)) {
    stdioGiven = true;
    auto kind = [&](Expr *e, unsigned i) -> std::optional<unsigned> {
      if (isa<UndefinedExpr>(e)) {
        return ChildPipe;
      }
      if (auto *n = dyn_cast<IntLitExpr>(e)) {
        n->type = Type::intTy();
        if (n->value > 0x7fffffff) {
          std::string message = ErrorTextChildStdio::noFd(n->value);
          this->state.getDiagnostics().error(e->loc(), message);
          return std::nullopt;
        }
        return ChildInherit + unsigned(n->value);
      }
      auto *lit = dyn_cast<StringLitExpr>(e);
      if (!lit) {
        std::string message = ErrorTextChildStdio::literal();
        this->state.getDiagnostics().error(e->loc(), message);
        this->exprChecker->checkExpr(e);
        return std::nullopt;
      }
      lit->type = string;
      std::string text = ToUtf8::toUtf8(lit->value);
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
      this->state.getDiagnostics().error(e->loc(), message);

      return std::nullopt;
    };
    if (auto *list = dyn_cast<ArrayLitExpr>(value)) {
      if (list->elements.size() > 3) {
        std::string message = ErrorTextChildStdio::three();
        this->state.getDiagnostics().error(list->elements[3].expr->loc(), message);
        failed = true;
      }
      for (size_t i = 0; i < list->elements.size() && i < 3; ++i) {
        const ArrayLitExpr::Element &element = list->elements[i];
        std::optional<unsigned> k = element.spread ? std::nullopt : kind(element.expr, unsigned(i));
        if (element.spread) {
          std::string message = ErrorTextChildStdio::spread();
          this->state.getDiagnostics().error(element.expr->loc(), message);
        }
        if (!k) {
          failed = true;
        } else {
          stdio[i] = *k;
        }
      }
    } else if (isa<StringLitExpr>(value)) {
      for (unsigned i = 0; i < 3; ++i) {
        std::optional<unsigned> k = kind(value, i);
        if (!k) {
          failed = true;
          break;
        }
        stdio[i] = *k;
      }
    } else {
      std::string message = ErrorTextChildOption::stdio(fn);
      this->state.getDiagnostics().error(value->loc(), message);
      this->exprChecker->checkExpr(value);
      failed = true;
    }
  }

  auto option = [&](llvm::StringRef name, const Type *type) {
    Expr *value = options.lookup(name);

    return value ? this->argumentChecker->checkArg(value, Type::optional(type)) : absent(type);
  };
  if (ChildChecker::hasZero(options.lookup("cwd"))) {
    std::string message = ErrorTextChildRun::zeroCwd();
    this->state.getDiagnostics().error(options.lookup("cwd")->loc(), message);
    failed = true;
  }
  for (llvm::StringRef limit : {"timeout", "maxBuffer"}) {
    Expr *value = options.lookup(limit);
    auto *unary = value ? dyn_cast<UnaryExpr>(value) : nullptr;
    if (unary && unary->op == UnaryOp::Neg &&
        (isa<IntLitExpr>(unary->operand) || isa<FloatLitExpr>(unary->operand))) {
      std::string message = ErrorTextChildRun::negative(limit);
      this->state.getDiagnostics().error(value->loc(), message);
      failed = true;
    }
  }
  if (NodeCheckHelpers::targetIsWindows()) {
    for (llvm::StringRef id : {"uid", "gid"}) {
      if (Expr *value = options.lookup(id); value && !isa<UndefinedExpr>(value)) {
        std::string message = ErrorTextChildRun::unixId(id);
        this->state.getDiagnostics().error(value->loc(), message);
        failed = true;
      }
    }
  }
  if (auto *lit = dyn_cast_or_null<StringLitExpr>(options.lookup("killSignal"));
      lit && !ChildChecker::knownSignal(ToUtf8::toUtf8(lit->value))) {
    std::string written = ToUtf8::toUtf8(lit->value);
    std::string message = ErrorTextChildRun::noSignal(written);
    this->state.getDiagnostics().error(lit->loc(), message);
    failed = true;
  }
  args[ChildCwd] = option("cwd", string);
  args[ChildEnv] = option("env", Type::dict(Type::DictKind::Record, string, string));
  args[ChildTimeout] = option("timeout", Type::doubleTy());
  args[ChildMaxBuffer] = option("maxBuffer", Type::doubleTy());
  args[ChildWindowsHide] = option("windowsHide", Type::boolTy());
  args[ChildVerbatim] = option("windowsVerbatimArguments", Type::boolTy());
  args[ChildDetached] = option("detached", Type::boolTy());
  args[ChildArgv0] = option("argv0", string);
  args[ChildUid] = option("uid", Type::intTy());
  args[ChildGid] = option("gid", Type::intTy());

  if (Expr *value = options.lookup("input")) {
    Expr *input = this->exprChecker->checkExpr(value, string);
    const Type *t = input->type->nonOptional();
    if (t->isStringLiteral()) {
      input = this->typeRelations->coerce(input, string);
    }
    t = input->type->nonOptional();
    if (t->isString() || t->isByteView()) {
      args[ChildInput] = this->typeRelations->coerce(input, Type::optional(t));
      if (t->isString() && encoding == ChildBytes && options.lookup("encoding") &&
          !isa<UndefinedExpr>(options.lookup("encoding"))) {
        std::string message = ErrorTextChildOption::bufferText();
        this->state.getDiagnostics().error(input->loc(), message);
        failed = true;
      }
    } else if (!t->isError()) {
      std::string message = ErrorTextChildOption::input(fn, input->type->name());
      this->state.getDiagnostics().error(input->loc(), message);
      failed = true;
    } else {
      failed = true;
    }
  } else {
    args[ChildInput] = absent(string);
  }
  if (Expr *value = options.lookup("killSignal")) {
    Expr *signal = this->exprChecker->checkExpr(value, string);
    const Type *t = signal->type->nonOptional();
    if (t->isStringLiteral() || t->isString()) {
      args[ChildKillSignal] = this->typeRelations->coerce(signal, Type::optional(string));
    } else if (t->isInt() || (t->isLiteral() && t->literalBase()->isInt())) {
      args[ChildKillSignal] = this->typeRelations->coerce(signal, Type::optional(Type::intTy()));
    } else {
      if (!t->isError()) {
        std::string message = ErrorTextChildOption::killSignal(signal->type->name());
        this->state.getDiagnostics().error(signal->loc(), message);
      }
      failed = true;
    }
  } else {
    args[ChildKillSignal] = absent(string);
  }
  bool viaShell = exec;
  if (Expr *value = options.lookup("shell")) {
    if (auto *lit = dyn_cast<BoolLitExpr>(value)) {
      viaShell = viaShell || lit->value;
    }
    Expr *shell = this->exprChecker->checkExpr(value, string);
    const Type *t = shell->type->nonOptional();
    bool isBool = t->isBool() || (t->isLiteral() && t->literalBase()->isBool());
    if (t->isStringLiteral() || t->isString()) {
      viaShell = true;
      args[ChildShell] = this->typeRelations->coerce(shell, Type::optional(string));
    } else if (isBool && exec) {
      if (auto *lit = dyn_cast<BoolLitExpr>(shell); lit && lit->value) {
        args[ChildShell] = absent(string);
      } else {
        std::string message = ErrorTextChildOption::execShell();
        this->state.getDiagnostics().error(shell->loc(), message);
        failed = true;
      }
    } else if (isBool && !isa<BoolLitExpr>(value)) {
      std::string message = ErrorTextChildOption::shellFlag(fn);
      this->state.getDiagnostics().error(shell->loc(), message);
      failed = true;
    } else if (isBool) {
      args[ChildShell] = this->typeRelations->coerce(shell, Type::optional(Type::boolTy()));
    } else {
      if (!t->isError()) {
        std::string message = ErrorTextChildOption::shell(shell->type->name());
        this->state.getDiagnostics().error(shell->loc(), message);
      }
      failed = true;
    }
  } else {
    args[ChildShell] = absent(string);
  }
  if (failed) {
    return call;
  }

  args[ChildStdin] = intLit(stdio[0]);
  args[ChildStdout] = intLit(stdio[1]);
  args[ChildStderr] = intLit(stdio[2]);
  args[ChildStdioGiven] = intLit(stdioGiven ? 1 : 0);
  args[ChildEncoding] = intLit(encoding);
  call->args = std::move(args);
  call->spread.clear();

  bool textOutput = encoding != ChildBytes;
  const Type *text = textOutput ? string : ChildChecker::bufferType();
  const Type *failure = this->childErrorTypes->errorOf(fn, textOutput, viaShell);
  auto output = [&](unsigned i) { return stdio[i] == ChildPipe ? text : Type::optional(text); };
  if (fn == "spawnSync") {
    call->target = BuiltinCall{.builtin = Builtin::ChildSpawnSync};
    call->type = Type::result(this->childErrorTypes->spawnResult(textOutput), failure);
  } else {
    call->target =
        BuiltinCall{.builtin = exec ? Builtin::ChildExecSync : Builtin::ChildExecFileSync};
    call->type = Type::result(output(1), failure);
  }

  return call;
}

}
