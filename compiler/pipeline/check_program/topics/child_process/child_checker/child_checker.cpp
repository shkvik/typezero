#include "compiler/pipeline/check_program/topics/child_process/child_checker/child_checker.h"
#include "compiler/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "compiler/pipeline/check_program/topics/bytes/bytes_checker/bytes_checker.h"
#include "compiler/pipeline/check_program/topics/child_process/child_error_types/child_error_types.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/fs/fs_checker/fs_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

std::string ChildChecker::utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
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
  if (targetIsWindows()) {
    return llvm::is_contained(windows, name);
  }

  return llvm::is_contained(posix, name);
}

bool ChildChecker::hasZero(const Expr *expr) {
  auto *lit = llvm::dyn_cast_or_null<StringLitExpr>(expr);

  return lit && lit->value.find(u'\0') != std::u16string::npos;
}

ChildChecker::ChildChecker(CheckState &owner)
    : state(owner) {}

Expr *ChildChecker::checkChildCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  call->type = Type::error();
  if (fn != "spawnSync" && fn != "execFileSync" && fn != "execSync") {
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    return call;
  }
  bool exec = fn == "execSync";
  bool rejected = CallChecker(this->state).rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : ArgumentChecker(this->state).checkArity(call, 1, exec ? 2 : 3);
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

    return TypeRelations(this->state).convert(none, Type::optional(type));
  };

  std::vector<Expr *> args(ChildArgCount, nullptr);
  args[ChildFile] = call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], string);
  bool failed = false;
  if (ChildChecker::hasZero(call->args[0])) {
    this->state.getDiagnostics().error(call->args[0]->loc(),
                                       "could not run it: the " +
                                           std::string(exec ? "command" : "program name") +
                                           " has a zero character in it");
    failed = true;
  } else if (auto *lit = dyn_cast<StringLitExpr>(call->args[0]);
             lit && lit->value.empty() && !exec) {
    this->state.getDiagnostics().error(call->args[0]->loc(),
                                       "could not run a program: its name is empty");
    failed = true;
  }
  size_t optionsAt = 1;
  if (!exec && call->args.size() > 1 && !isa<ObjectLitExpr>(call->args[1])) {
    if (!isa<UndefinedExpr>(call->args[1])) {
      if (auto *list = dyn_cast<ArrayLitExpr>(call->args[1])) {
        for (size_t i = 0; i < list->elements.size(); ++i) {
          if (!list->elements[i].spread && ChildChecker::hasZero(list->elements[i].expr)) {
            this->state.getDiagnostics().error(list->elements[i].expr->loc(),
                                               "could not run it: args[" + std::to_string(i) +
                                                   "] has a zero character in it");
            failed = true;
          }
        }
      }
      args[ChildArgs] = call->args[1] =
          ArgumentChecker(this->state).checkArg(call->args[1], Type::array(string));
    }
    optionsAt = 2;
  } else if (!exec && call->args.size() > 2) {
    this->state.getDiagnostics().error(
        call->args[2]->loc(), "the options of '" + fn + "' come after its arguments, e.g. '" + fn +
                                  "(file, [\"--version\"], { cwd })'");
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
  if (!FsChecker(this->state).nodeOptions(call, optionsAt, fn, allowed, options)) {
    return call;
  }

  unsigned encoding = ChildBytes;
  if (Expr *value = options.lookup("encoding"); value && !isa<UndefinedExpr>(value)) {
    auto *lit = dyn_cast<StringLitExpr>(value);
    if (!lit) {
      this->state.getDiagnostics().error(
          value->loc(), "the option 'encoding' of '" + fn +
                            "' is written as a literal, e.g. 'utf8': it decides whether "
                            "the output is a string or a Buffer");
      ExprChecker(this->state).checkExpr(value);
      failed = true;
    } else if (lit->value != u"buffer") {
      lit->type = string;
      Expr *number = BytesChecker(this->state).checkBufferEncoding(lit);
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
          this->state.getDiagnostics().error(e->loc(), "there is no file descriptor " +
                                                           std::to_string(n->value));
          return std::nullopt;
        }
        return ChildInherit + unsigned(n->value);
      }
      auto *lit = dyn_cast<StringLitExpr>(e);
      if (!lit) {
        this->state.getDiagnostics().error(
            e->loc(), "each stream of 'stdio' is written as a literal: \"pipe\", "
                      "\"inherit\", \"ignore\" or a file descriptor such as 2");
        ExprChecker(this->state).checkExpr(e);
        return std::nullopt;
      }
      lit->type = string;
      std::string text = ChildChecker::utf8(lit->value);
      if (text == "pipe" || text == "overlapped") {
        return ChildPipe;
      }
      if (text == "inherit") {
        return ChildInherit + i;
      }
      if (text == "ignore") {
        return ChildIgnore;
      }
      if (text == "ipc") {
        this->state.getDiagnostics().error(
            e->loc(), "'ipc' is the channel to another Node that fork starts, which tz "
                      "does not have (docs/process.md, section 4)");
      } else {
        this->state.getDiagnostics().error(
            e->loc(), "unknown stdio '" + text +
                          "'; a stream is \"pipe\", \"inherit\", \"ignore\" or a file "
                          "descriptor such as 2");
      }

      return std::nullopt;
    };
    if (auto *list = dyn_cast<ArrayLitExpr>(value)) {
      if (list->elements.size() > 3) {
        this->state.getDiagnostics().error(
            list->elements[3].expr->loc(),
            "only stdin, stdout and stderr are supported in 'stdio'; more streams come "
            "with the async functions (docs/process.md, step 2)");
        failed = true;
      }
      for (size_t i = 0; i < list->elements.size() && i < 3; ++i) {
        const ArrayLitExpr::Element &element = list->elements[i];
        std::optional<unsigned> k = element.spread ? std::nullopt : kind(element.expr, unsigned(i));
        if (element.spread) {
          this->state.getDiagnostics().error(element.expr->loc(),
                                             "write each stream of 'stdio' in the literal");
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
      this->state.getDiagnostics().error(
          value->loc(), "the option 'stdio' of '" + fn +
                            "' is written as a literal, e.g. \"inherit\" or [\"pipe\", "
                            "\"inherit\", \"pipe\"]: whether there is output depends "
                            "on it");
      ExprChecker(this->state).checkExpr(value);
      failed = true;
    }
  }

  auto option = [&](llvm::StringRef name, const Type *type) {
    Expr *value = options.lookup(name);

    return value ? ArgumentChecker(this->state).checkArg(value, Type::optional(type))
                 : absent(type);
  };
  if (ChildChecker::hasZero(options.lookup("cwd"))) {
    this->state.getDiagnostics().error(options.lookup("cwd")->loc(),
                                       "could not run it: cwd has a zero character in it");
    failed = true;
  }
  for (llvm::StringRef limit : {"timeout", "maxBuffer"}) {
    Expr *value = options.lookup(limit);
    auto *unary = value ? dyn_cast<UnaryExpr>(value) : nullptr;
    if (unary && unary->op == UnaryOp::Neg &&
        (isa<IntLitExpr>(unary->operand) || isa<FloatLitExpr>(unary->operand))) {
      this->state.getDiagnostics().error(value->loc(),
                                         "could not run it: " + limit.str() + " must be 0 or more");
      failed = true;
    }
  }
  if (targetIsWindows()) {
    for (llvm::StringRef id : {"uid", "gid"}) {
      if (Expr *value = options.lookup(id); value && !isa<UndefinedExpr>(value)) {
        this->state.getDiagnostics().error(value->loc(),
                                           "'" + id.str() +
                                               "' works only on Linux and macOS: Windows has "
                                               "no user and group numbers");
        failed = true;
      }
    }
  }
  if (auto *lit = dyn_cast_or_null<StringLitExpr>(options.lookup("killSignal"));
      lit && !ChildChecker::knownSignal(ChildChecker::utf8(lit->value))) {
    this->state.getDiagnostics().error(lit->loc(), "could not run it: there is no signal " +
                                                       ChildChecker::utf8(lit->value) +
                                                       " (killSignal)");
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
    Expr *input = ExprChecker(this->state).checkExpr(value, string);
    const Type *t = input->type->nonOptional();
    if (t->isStringLiteral()) {
      input = TypeRelations(this->state).coerce(input, string);
    }
    t = input->type->nonOptional();
    if (t->isString() || t->isByteView()) {
      args[ChildInput] = TypeRelations(this->state).coerce(input, Type::optional(t));
      if (t->isString() && encoding == ChildBytes && options.lookup("encoding") &&
          !isa<UndefinedExpr>(options.lookup("encoding"))) {
        this->state.getDiagnostics().error(
            input->loc(), "text input with the encoding 'buffer' is a TypeError in Node "
                          "(Unknown encoding: buffer); pass its bytes, "
                          "'Buffer.from(text)', or drop the encoding");
        failed = true;
      }
    } else if (!t->isError()) {
      this->state.getDiagnostics().error(
          input->loc(), "'input' of '" + fn +
                            "' is the text or the bytes the program reads (a string, a "
                            "Buffer or a Uint8Array); found '" +
                            input->type->name() + "'");
      failed = true;
    } else {
      failed = true;
    }
  } else {
    args[ChildInput] = absent(string);
  }
  if (Expr *value = options.lookup("killSignal")) {
    Expr *signal = ExprChecker(this->state).checkExpr(value, string);
    const Type *t = signal->type->nonOptional();
    if (t->isStringLiteral() || t->isString()) {
      args[ChildKillSignal] = TypeRelations(this->state).coerce(signal, Type::optional(string));
    } else if (t->isInt() || (t->isLiteral() && t->literalBase()->isInt())) {
      args[ChildKillSignal] =
          TypeRelations(this->state).coerce(signal, Type::optional(Type::intTy()));
    } else {
      if (!t->isError()) {
        this->state.getDiagnostics().error(
            signal->loc(), "'killSignal' is the name of a signal, e.g. \"SIGKILL\", or "
                           "its number; found '" +
                               signal->type->name() + "'");
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
    Expr *shell = ExprChecker(this->state).checkExpr(value, string);
    const Type *t = shell->type->nonOptional();
    bool isBool = t->isBool() || (t->isLiteral() && t->literalBase()->isBool());
    if (t->isStringLiteral() || t->isString()) {
      viaShell = true;
      args[ChildShell] = TypeRelations(this->state).coerce(shell, Type::optional(string));
    } else if (isBool && exec) {
      if (auto *lit = dyn_cast<BoolLitExpr>(shell); lit && lit->value) {
        args[ChildShell] = absent(string);
      } else {
        this->state.getDiagnostics().error(
            shell->loc(), "'execSync' always runs the command through a shell; "
                          "'shell' may name another one, e.g. shell: \"powershell.exe\", "
                          "and to run a program without one, use 'execFileSync'");
        failed = true;
      }
    } else if (isBool && !isa<BoolLitExpr>(value)) {
      this->state.getDiagnostics().error(
          shell->loc(), "the option 'shell' of '" + fn +
                            "' is written as a literal, true or false: what can go wrong, "
                            "and so the type of the error, depends on it");
      failed = true;
    } else if (isBool) {
      args[ChildShell] = TypeRelations(this->state).coerce(shell, Type::optional(Type::boolTy()));
    } else {
      if (!t->isError()) {
        this->state.getDiagnostics().error(
            shell->loc(),
            "'shell' is true, false or the path of a shell; found '" + shell->type->name() + "'");
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
  const Type *failure = ChildErrorTypes(this->state).errorOf(fn, textOutput, viaShell);
  auto output = [&](unsigned i) { return stdio[i] == ChildPipe ? text : Type::optional(text); };
  if (fn == "spawnSync") {
    call->target = BuiltinCall{.builtin = Builtin::ChildSpawnSync};
    call->type = Type::result(ChildErrorTypes(this->state).spawnResult(textOutput), failure);
  } else {
    call->target =
        BuiltinCall{.builtin = exec ? Builtin::ChildExecSync : Builtin::ChildExecFileSync};
    call->type = Type::result(output(1), failure);
  }

  return call;
}

}
