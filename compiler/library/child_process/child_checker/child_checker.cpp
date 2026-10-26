#include "compiler/library/child_process/child_checker/child_checker.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/fs/fs_checker/fs_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
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

const Type *outputUnion() {
  return Type::unionOf({Type::string(), bufferType(), Type::undefinedTy()});
}

}

ChildChecker::ChildChecker(Checker &checker) : checker_(checker) {}

void ChildChecker::declareExecSyncError() {
  auto *cls = checker_.ctx_.make<ClassDecl>(llvm::SMLoc(), "ExecSyncError");
  cls->builtin = true;
  cls->resolved = cls->declared = true;
  cls->parent = checker_.errorClass_;
  checker_.errorClass_->children.push_back(cls);
  Type *type = Type::classType("ExecSyncError", cls);
  cls->type = type;
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
  const Type *error = checker_.errorClass_->type;
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
  checker_.classBases().addBase(type, error);
  checker_.execSyncErrorClass_ = cls;
}

Expr *ChildChecker::checkChildCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  call->type = Type::error();
  if (fn != "spawnSync" && fn != "execFileSync" && fn != "execSync") {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  bool exec = fn == "execSync";
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, loc, 1, exec ? 2 : 3)) {
    return call;
  }
  const Type *string = Type::string();
  auto intLit = [&](uint64_t value) {
    auto *lit = checker_.ctx_.make<IntLitExpr>(loc, value);
    lit->type = Type::intTy();

    return lit;
  };
  auto absent = [&](const Type *type) {
    auto *none = checker_.ctx_.make<UndefinedExpr>(loc);
    none->type = Type::undefinedTy();

    return checker_.typeRelations().convert(none, Type::optional(type));
  };

  std::vector<Expr *> args(ChildArgCount, nullptr);
  args[ChildFile] = call->args[0] = checker_.argumentChecker().checkArg(call->args[0], string);
  size_t optionsAt = 1;
  if (!exec && call->args.size() > 1 && !isa<ObjectLitExpr>(call->args[1])) {
    if (!isa<UndefinedExpr>(call->args[1])) {
      args[ChildArgs] = call->args[1] =
          checker_.argumentChecker().checkArg(call->args[1], Type::array(string));
    }
    optionsAt = 2;
  } else if (!exec && call->args.size() > 2) {
    checker_.diags_.error(call->args[2]->loc(), "the options of '" + fn +
                                                    "' come after its arguments, e.g. '" + fn +
                                                    "(file, [\"--version\"], { cwd })'");
    return call;
  }
  if (!args[ChildArgs]) {
    auto *none = checker_.ctx_.make<ArrayLitExpr>(loc);
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
  if (!checker_.fsChecker().nodeOptions(call, optionsAt, fn, allowed, options)) {
    return call;
  }

  bool failed = false;
  unsigned encoding = ChildBytes;
  if (Expr *value = options.lookup("encoding"); value && !isa<UndefinedExpr>(value)) {
    auto *lit = dyn_cast<StringLitExpr>(value);
    if (!lit) {
      checker_.diags_.error(value->loc(),
                            "the option 'encoding' of '" + fn +
                                "' is written as a literal, e.g. 'utf8': it decides whether "
                                "the output is a string or a Buffer");
      checker_.exprChecker().checkExpr(value);
      failed = true;
    } else if (lit->value != u"buffer") {
      lit->type = string;
      Expr *number = checker_.bytesChecker().checkBufferEncoding(lit);
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
          checker_.diags_.error(e->loc(),
                                "there is no file descriptor " + std::to_string(n->value));
          return std::nullopt;
        }
        return ChildInherit + unsigned(n->value);
      }
      auto *lit = dyn_cast<StringLitExpr>(e);
      if (!lit) {
        checker_.diags_.error(e->loc(), "each stream of 'stdio' is written as a literal: \"pipe\", "
                                        "\"inherit\", \"ignore\" or a file descriptor such as 2");
        checker_.exprChecker().checkExpr(e);
        return std::nullopt;
      }
      lit->type = string;
      std::string text = utf8(lit->value);
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
        checker_.diags_.error(e->loc(),
                              "'ipc' is the channel to another Node that fork starts, which tz "
                              "does not have (docs/process.md, section 4)");
      } else {
        checker_.diags_.error(e->loc(),
                              "unknown stdio '" + text +
                                  "'; a stream is \"pipe\", \"inherit\", \"ignore\" or a file "
                                  "descriptor such as 2");
      }

      return std::nullopt;
    };
    if (auto *list = dyn_cast<ArrayLitExpr>(value)) {
      if (list->elements.size() > 3) {
        checker_.diags_.error(
            list->elements[3].expr->loc(),
            "only stdin, stdout and stderr are supported in 'stdio'; more streams come "
            "with the async functions (docs/process.md, step 2)");
        failed = true;
      }
      for (size_t i = 0; i < list->elements.size() && i < 3; ++i) {
        const ArrayLitExpr::Element &element = list->elements[i];
        std::optional<unsigned> k = element.spread ? std::nullopt : kind(element.expr, unsigned(i));
        if (element.spread) {
          checker_.diags_.error(element.expr->loc(), "write each stream of 'stdio' in the literal");
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
      checker_.diags_.error(value->loc(),
                            "the option 'stdio' of '" + fn +
                                "' is written as a literal, e.g. \"inherit\" or [\"pipe\", "
                                "\"inherit\", \"pipe\"]: whether there is output depends "
                                "on it");
      checker_.exprChecker().checkExpr(value);
      failed = true;
    }
  }

  auto option = [&](llvm::StringRef name, const Type *type) {
    Expr *value = options.lookup(name);

    return value ? checker_.argumentChecker().checkArg(value, Type::optional(type)) : absent(type);
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

  if (Expr *value = options.lookup("input")) {
    Expr *input = checker_.exprChecker().checkExpr(value, string);
    const Type *t = input->type->nonOptional();
    if (t->isStringLiteral()) {
      input = checker_.typeRelations().coerce(input, string);
    }
    t = input->type->nonOptional();
    if (t->isString() || t->isByteView()) {
      args[ChildInput] = checker_.typeRelations().coerce(input, Type::optional(t));
      if (t->isString() && encoding == ChildBytes && options.lookup("encoding") &&
          !isa<UndefinedExpr>(options.lookup("encoding"))) {
        checker_.diags_.error(input->loc(),
                              "text input with the encoding 'buffer' is a TypeError in Node "
                              "(Unknown encoding: buffer); pass its bytes, "
                              "'Buffer.from(text)', or drop the encoding");
        failed = true;
      }
    } else if (!t->isError()) {
      checker_.diags_.error(input->loc(),
                            "'input' of '" + fn +
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
    Expr *signal = checker_.exprChecker().checkExpr(value, string);
    const Type *t = signal->type->nonOptional();
    if (t->isStringLiteral() || t->isString()) {
      args[ChildKillSignal] = checker_.typeRelations().coerce(signal, Type::optional(string));
    } else if (t->isInt() || (t->isLiteral() && t->literalBase()->isInt())) {
      args[ChildKillSignal] =
          checker_.typeRelations().coerce(signal, Type::optional(Type::intTy()));
    } else {
      if (!t->isError()) {
        checker_.diags_.error(signal->loc(),
                              "'killSignal' is the name of a signal, e.g. \"SIGKILL\", or "
                              "its number; found '" +
                                  signal->type->name() + "'");
      }
      failed = true;
    }
  } else {
    args[ChildKillSignal] = absent(string);
  }
  if (Expr *value = options.lookup("shell")) {
    Expr *shell = checker_.exprChecker().checkExpr(value, string);
    const Type *t = shell->type->nonOptional();
    bool isBool = t->isBool() || (t->isLiteral() && t->literalBase()->isBool());
    if (t->isStringLiteral() || t->isString()) {
      args[ChildShell] = checker_.typeRelations().coerce(shell, Type::optional(string));
    } else if (isBool && exec) {
      if (auto *lit = dyn_cast<BoolLitExpr>(shell); lit && lit->value) {
        args[ChildShell] = absent(string);
      } else {
        checker_.diags_.error(shell->loc(),
                              "'execSync' always runs the command through a shell; "
                              "'shell' may name another one, e.g. shell: \"powershell.exe\", "
                              "and to run a program without one, use 'execFileSync'");
        failed = true;
      }
    } else if (isBool) {
      args[ChildShell] = checker_.typeRelations().coerce(shell, Type::optional(Type::boolTy()));
    } else {
      if (!t->isError()) {
        checker_.diags_.error(shell->loc(),
                              "'shell' is true, false or the path of a shell; found '" +
                                  shell->type->name() + "'");
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

  const Type *text = encoding == ChildBytes ? bufferType() : string;
  auto output = [&](unsigned i) { return stdio[i] == ChildPipe ? text : Type::optional(text); };
  if (fn == "spawnSync") {
    call->builtin = Builtin::ChildSpawnSync;
    const Type *t = Type::object(
        {{"error", Type::optional(checker_.systemErrorClass_->type), false},
         {"status", Type::optional(Type::intTy()), false},
         {"signal", Type::optional(string), false},
         {"output", Type::array(Type::optional(text)), false},
         {"pid", Type::intTy(), false},
         {"stdout", output(1), false},
         {"stderr", output(2), false}},
        encoding == ChildBytes ? "SpawnSyncReturns<Buffer>" : "SpawnSyncReturns<string>");
    call->type = t;
  } else {
    call->builtin = exec ? Builtin::ChildExecSync : Builtin::ChildExecFileSync;
    call->type = output(1);
  }

  return call;
}

}
