// The synchronous functions of node:child_process (docs/process.md):
// spawnSync, execFileSync and execSync, as in Node.
//
// Their options are an object literal written in the call, as those of
// node:fs are: `encoding` decides whether the output is a string or a Buffer,
// and `stdio` whether there is any (a stream the child inherits gives none),
// so those two must be literals. The others are values of any expression,
// `T | undefined` too. Each call gets the arguments ChildArg lists, the
// absent options undefined, and runtime/child.c runs the program.
//
// spawnSync never throws for the program: it gives an object with what
// happened, as Node does. execSync and execFileSync give what the program
// printed, and throw when it fails: a SystemError when it could not start
// (ENOENT...), as node:fs throws, and an ExecSyncError when it ran and failed
// (its exit code is not 0, it ran out of time, it printed too much), with
// the fields Node gives that error: status, signal, output, pid, stdout and
// stderr.

#include "sema/sema.h"

#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);
  return out;
}

const Type *bufferType() { return Type::bytesType(Type::Bytes::Buffer); }

// What stdout and stderr of an ExecSyncError hold: a string with an
// encoding, a Buffer without one, undefined for a stream the child did not
// write to a pipe.
const Type *outputUnion() {
  return Type::unionOf({Type::string(), bufferType(), Type::undefinedTy()});
}

} // namespace

void Sema::declareExecSyncError() {
  auto *cls = ctx_.make<ClassDecl>(llvm::SMLoc(), "ExecSyncError");
  cls->builtin = true;
  cls->resolved = cls->declared = true;
  cls->parent = errorClass_;
  errorClass_->children.push_back(cls);
  Type *type = Type::classType("ExecSyncError", cls);
  cls->type = type;
  // The fields Node's error has, in the order it prints them. The first five
  // are those of an error of the system the program ran into (ETIMEDOUT,
  // ENOBUFS), undefined when it only failed; then the result of spawnSync.
  const Type *s = Type::optional(Type::string());
  const std::pair<llvm::StringRef, const Type *> own[] = {
      {"errno", Type::optional(Type::intTy())},
      {"code", s},
      {"syscall", s},
      {"path", s},
      {"spawnargs", Type::optional(Type::array(Type::string()))},
      {"status", Type::optional(Type::intTy())},
      {"signal", s},
      {"output", Type::array(outputUnion())},
      {"pid", Type::intTy()},
      {"stdout", outputUnion()},
      {"stderr", outputUnion()}};
  const Type *error = errorClass_->type;
  std::vector<Field> fields(error->fields().begin(), error->fields().end());
  for (const auto &[name, fieldType] : own) {
    ClassField f;
    f.name = name;
    f.resolved = fieldType;
    f.optional = fieldType->isOptional();
    cls->fields.push_back(f);
    fields.push_back({name.str(), fieldType, false});
  }
  type->defineFields(std::move(fields));
  addBase(type, error);
  execSyncErrorClass_ = cls;
}

Expr *Sema::checkChildCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  call->type = Type::error();
  if (fn != "spawnSync" && fn != "execFileSync" && fn != "execSync") {
    // Reported at the import.
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  bool exec = fn == "execSync";
  if (rejectSpread(call) || !checkArity(call, loc, 1, exec ? 2 : 3))
    return call;
  const Type *string = Type::string();
  auto intLit = [&](uint64_t value) {
    auto *lit = ctx_.make<IntLitExpr>(loc, value);
    lit->type = Type::intTy();
    return lit;
  };
  // An option the call does not give: undefined of its type.
  auto absent = [&](const Type *type) {
    auto *none = ctx_.make<UndefinedExpr>(loc);
    none->type = Type::undefinedTy();
    return convert(none, Type::optional(type));
  };

  std::vector<Expr *> args(ChildArgCount, nullptr);
  args[ChildFile] = call->args[0] = checkArg(call->args[0], string);
  // The arguments, which may be left out before the options, as in Node:
  // `execFileSync("git", { cwd })`.
  size_t optionsAt = 1;
  if (!exec && call->args.size() > 1 && !isa<ObjectLitExpr>(call->args[1])) {
    if (!isa<UndefinedExpr>(call->args[1]))
      args[ChildArgs] = call->args[1] = checkArg(call->args[1], Type::array(string));
    optionsAt = 2;
  } else if (!exec && call->args.size() > 2) {
    diags_.error(call->args[2]->loc(), "the options of '" + fn +
                                           "' come after its arguments, e.g. '" + fn +
                                           "(file, [\"--version\"], { cwd })'");
    return call;
  }
  if (!args[ChildArgs]) {
    auto *none = ctx_.make<ArrayLitExpr>(loc);
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
  if (!nodeOptions(call, optionsAt, fn, allowed, options))
    return call;

  // The options that decide the types, which are literals.
  bool failed = false;
  unsigned encoding = ChildBytes;
  if (Expr *value = options.lookup("encoding"); value && !isa<UndefinedExpr>(value)) {
    auto *lit = dyn_cast<StringLitExpr>(value);
    if (!lit) {
      diags_.error(value->loc(), "the option 'encoding' of '" + fn +
                                     "' is written as a literal, e.g. 'utf8': it decides whether "
                                     "the output is a string or a Buffer");
      checkExpr(value);
      failed = true;
    } else if (lit->value != u"buffer") {
      lit->type = string;
      Expr *number = checkBufferEncoding(lit);
      if (number->type->isError())
        failed = true;
      else
        encoding = 1 + unsigned(cast<IntLitExpr>(number)->value);
    }
  }
  unsigned stdio[3] = {ChildPipe, ChildPipe, ChildPipe};
  bool stdioGiven = false;
  if (Expr *value = options.lookup("stdio"); value && !isa<UndefinedExpr>(value)) {
    stdioGiven = true;
    // One of them: "pipe", "inherit", "ignore", an fd, or undefined for a
    // pipe.
    auto kind = [&](Expr *e, unsigned i) -> std::optional<unsigned> {
      if (isa<UndefinedExpr>(e))
        return ChildPipe;
      if (auto *n = dyn_cast<IntLitExpr>(e)) {
        n->type = Type::intTy();
        if (n->value > 0x7fffffff) {
          diags_.error(e->loc(), "there is no file descriptor " + std::to_string(n->value));
          return std::nullopt;
        }
        return ChildInherit + unsigned(n->value);
      }
      auto *lit = dyn_cast<StringLitExpr>(e);
      if (!lit) {
        diags_.error(e->loc(), "each stream of 'stdio' is written as a literal: \"pipe\", "
                               "\"inherit\", \"ignore\" or a file descriptor such as 2");
        checkExpr(e);
        return std::nullopt;
      }
      lit->type = string;
      std::string text = utf8(lit->value);
      if (text == "pipe" || text == "overlapped")
        return ChildPipe;
      if (text == "inherit")
        return ChildInherit + i;
      if (text == "ignore")
        return ChildIgnore;
      if (text == "ipc")
        diags_.error(e->loc(), "'ipc' is the channel to another Node that fork starts, which tz "
                               "does not have (docs/process.md, section 4)");
      else
        diags_.error(e->loc(), "unknown stdio '" + text +
                                   "'; a stream is \"pipe\", \"inherit\", \"ignore\" or a file "
                                   "descriptor such as 2");
      return std::nullopt;
    };
    if (auto *list = dyn_cast<ArrayLitExpr>(value)) {
      if (list->elements.size() > 3) {
        diags_.error(list->elements[3].expr->loc(),
                     "only stdin, stdout and stderr are supported in 'stdio'; more streams come "
                     "with the async functions (docs/process.md, step 2)");
        failed = true;
      }
      for (size_t i = 0; i < list->elements.size() && i < 3; ++i) {
        const ArrayLitExpr::Element &element = list->elements[i];
        std::optional<unsigned> k = element.spread ? std::nullopt : kind(element.expr, unsigned(i));
        if (element.spread)
          diags_.error(element.expr->loc(), "write each stream of 'stdio' in the literal");
        if (!k)
          failed = true;
        else
          stdio[i] = *k;
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
      diags_.error(value->loc(), "the option 'stdio' of '" + fn +
                                     "' is written as a literal, e.g. \"inherit\" or [\"pipe\", "
                                     "\"inherit\", \"pipe\"]: whether there is output depends "
                                     "on it");
      checkExpr(value);
      failed = true;
    }
  }

  // The others, of any expression.
  auto option = [&](llvm::StringRef name, const Type *type) {
    Expr *value = options.lookup(name);
    return value ? checkArg(value, Type::optional(type)) : absent(type);
  };
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

  // input: text, written in the encoding of the call as Node writes it, or
  // the bytes of a view.
  if (Expr *value = options.lookup("input")) {
    Expr *input = checkExpr(value, string);
    const Type *t = input->type->nonOptional();
    if (t->isStringLiteral())
      input = coerce(input, string);
    t = input->type->nonOptional();
    if (t->isString() || t->isByteView()) {
      args[ChildInput] = coerce(input, Type::optional(t));
      if (t->isString() && encoding == ChildBytes && options.lookup("encoding") &&
          !isa<UndefinedExpr>(options.lookup("encoding"))) {
        // Node encodes the text with the encoding, and 'buffer' is none.
        diags_.error(input->loc(), "text input with the encoding 'buffer' is a TypeError in Node "
                                   "(Unknown encoding: buffer); pass its bytes, "
                                   "'Buffer.from(text)', or drop the encoding");
        failed = true;
      }
    } else if (!t->isError()) {
      diags_.error(input->loc(), "'input' of '" + fn +
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
  // killSignal: a name ("SIGKILL") or a number, checked when the program
  // runs, as in Node.
  if (Expr *value = options.lookup("killSignal")) {
    Expr *signal = checkExpr(value, string);
    const Type *t = signal->type->nonOptional();
    if (t->isStringLiteral() || t->isString()) {
      args[ChildKillSignal] = coerce(signal, Type::optional(string));
    } else if (t->isInt() || (t->isLiteral() && t->literalBase()->isInt())) {
      args[ChildKillSignal] = coerce(signal, Type::optional(Type::intTy()));
    } else {
      if (!t->isError())
        diags_.error(signal->loc(), "'killSignal' is the name of a signal, e.g. \"SIGKILL\", or "
                                    "its number; found '" +
                                        signal->type->name() + "'");
      failed = true;
    }
  } else {
    args[ChildKillSignal] = absent(string);
  }
  // shell: true for the system's (cmd.exe, /bin/sh), or the path of one.
  // execSync always goes through one: there it may only name it.
  if (Expr *value = options.lookup("shell")) {
    Expr *shell = checkExpr(value, string);
    const Type *t = shell->type->nonOptional();
    bool isBool = t->isBool() || (t->isLiteral() && t->literalBase()->isBool());
    if (t->isStringLiteral() || t->isString()) {
      args[ChildShell] = coerce(shell, Type::optional(string));
    } else if (isBool && exec) {
      if (auto *lit = dyn_cast<BoolLitExpr>(shell); lit && lit->value) {
        args[ChildShell] = absent(string);
      } else {
        diags_.error(shell->loc(), "'execSync' always runs the command through a shell; "
                                   "'shell' may name another one, e.g. shell: \"powershell.exe\", "
                                   "and to run a program without one, use 'execFileSync'");
        failed = true;
      }
    } else if (isBool) {
      args[ChildShell] = coerce(shell, Type::optional(Type::boolTy()));
    } else {
      if (!t->isError())
        diags_.error(shell->loc(), "'shell' is true, false or the path of a shell; found '" +
                                       shell->type->name() + "'");
      failed = true;
    }
  } else {
    args[ChildShell] = absent(string);
  }
  if (failed)
    return call;

  args[ChildStdin] = intLit(stdio[0]);
  args[ChildStdout] = intLit(stdio[1]);
  args[ChildStderr] = intLit(stdio[2]);
  args[ChildStdioGiven] = intLit(stdioGiven ? 1 : 0);
  args[ChildEncoding] = intLit(encoding);
  call->args = std::move(args);
  call->spread.clear();

  // What the program printed: a string with an encoding, a Buffer without
  // one; undefined for a stream that was no pipe.
  const Type *text = encoding == ChildBytes ? bufferType() : string;
  auto output = [&](unsigned i) { return stdio[i] == ChildPipe ? text : Type::optional(text); };
  if (fn == "spawnSync") {
    call->builtin = Builtin::ChildSpawnSync;
    // In the order Node prints them; `error` is there only when the program
    // could not start or did not end by itself.
    const Type *t = Type::object({{"error", Type::optional(systemErrorClass_->type), false},
                                  {"status", Type::optional(Type::intTy()), false},
                                  {"signal", Type::optional(string), false},
                                  {"output", Type::array(Type::optional(text)), false},
                                  {"pid", Type::intTy(), false},
                                  {"stdout", output(1), false},
                                  {"stderr", output(2), false}},
                                 encoding == ChildBytes ? "SpawnSyncReturns<Buffer>"
                                                        : "SpawnSyncReturns<string>");
    call->type = t;
  } else {
    call->builtin = exec ? Builtin::ChildExecSync : Builtin::ChildExecFileSync;
    call->type = output(1);
  }
  return call;
}

} // namespace tz
