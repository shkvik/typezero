#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/async/timer_checker/timer_checker.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/stream/stream_sources/stream_sources.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

std::u16string toUtf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

} // namespace

ProcessChecker::ProcessChecker(Sema &sema) : sema_(sema) {}

bool ProcessChecker::isProcessMember(const Expr *expr, llvm::StringRef name) const {
  auto *member = dyn_cast<MemberExpr>(expr);

  return member && !member->optional && member->name == name &&
         sema_.callChecker().isGlobalName(member->object, "process");
}

Expr *ProcessChecker::builtinCall(llvm::SMLoc loc, Expr *callee, Builtin builtin, std::vector<Expr *> args,
                        const Type *type) {
  auto *call = sema_.ctx_.make<CallExpr>(loc, callee, std::move(args));
  call->builtin = builtin;
  call->type = type;

  return call;
}

Expr *ProcessChecker::checkExitCode(Expr *code) {
  const Type *optionalInt = Type::optional(Type::intTy());
  code = sema_.exprChecker().checkExpr(code, Type::intTy());
  if (code->type->isOptional() || code->type->isUndefined()) {
    return sema_.typeRelations().coerce(code, optionalInt);
  }

  return sema_.typeRelations().convert(sema_.typeRelations().coerce(code, Type::intTy()), optionalInt);
}

// process.exit(), process.cwd(), process.stdout.write(), Date.now() and
// performance.now().
Expr *ProcessChecker::checkProcessCall(CallExpr *call, MemberExpr *member) {
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
  };
  llvm::StringRef name = member->name;

  bool isDate = sema_.callChecker().isGlobalName(member->object, "Date");
  if (isDate && name != "now") {
    return sema_.dateChecker().checkDateStatic(call, member); // Date.UTC, Date.parse (sema_date.cpp)
  }
  if (isDate || sema_.callChecker().isGlobalName(member->object, "performance")) {
    llvm::StringRef object = isDate ? "Date" : "performance";
    if (name != "now") {
      sema_.diags_.error(member->loc(), "'" + object + "." + name + "' is not supported yet; only '" +
                                      object + ".now()' is");
      checkArgsLoosely();
      return call;
    }
    if (sema_.callChecker().rejectSpread(call) || !sema_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
      return call;
    }
    // Date.now() gives whole milliseconds, performance.now() fractions too.
    call->builtin = isDate ? Builtin::DateNow : Builtin::PerformanceNow;
    call->type = Type::doubleTy();
    return call;
  }

  bool isStdout = isProcessMember(member->object, "stdout");
  if (isStdout || isProcessMember(member->object, "stderr")) {
    std::string stream = isStdout ? "process.stdout" : "process.stderr";
    // The stream of node:stream, where the program uses it as one
    // (sema_stream.cpp): it writes at once too.
    if (name != "isTTY" && sema_.libFunctions_.lookup("processStdout")) {
      return nullptr;
    }
    if (name != "write") {
      if (name == "isTTY") {
        sema_.diags_.error(member->loc(), "'" + stream + ".isTTY' is a boolean, not a function");
      } else {
        sema_.diags_.error(member->loc(),
                     "'" + stream + "." + name +
                         "' is not supported yet; only 'write(text)' and 'isTTY' are");
      }
      checkArgsLoosely();
      return call;
    }
    if (sema_.callChecker().rejectSpread(call) || !sema_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    // Text, or the bytes of a Buffer or a Uint8Array as they are; the result
    // is always true, as nothing waits to be written. A write with the
    // encoding or the callback of node is one of the stream of node:stream,
    // which the loader brings then.
    Expr *chunk = sema_.exprChecker().checkExpr(call->args[0], Type::string());
    const Type *type = chunk->type;
    if (!type->isBuffer() && type->bytes() != Type::Bytes::Uint8) {
      chunk = sema_.typeRelations().coerce(chunk, Type::string());
    }
    call->args[0] = chunk;
    call->builtin = isStdout ? Builtin::ProcessStdoutWrite : Builtin::ProcessStderrWrite;
    call->type = Type::boolTy();
    return call;
  }

  if (!sema_.callChecker().isGlobalName(member->object, "process")) {
    return nullptr;
  }
  if (sema_.callChecker().rejectSpread(call)) {
    return call;
  }
  if (name == "nextTick") {
    return sema_.timerChecker().checkNextTick(call, member); // sema_async.cpp
  }
  if (name == "exit") {
    // The program ends here, as in Node: `finally` blocks do not run, and
    // what is printed so far is written out. Without a code it ends with
    // process.exitCode.
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 1)) {
      return call;
    }
    if (!call->args.empty()) {
      call->args[0] = checkExitCode(call->args[0]);
    }
    call->builtin = Builtin::ProcessExit;
    call->type = Type::voidTy();
    return call;
  }
  if (name == "cwd") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
      return call;
    }
    call->builtin = Builtin::ProcessCwd;
    call->type = Type::string();
    return call;
  }
  static const llvm::StringLiteral properties[] = {"argv",     "env",    "exitCode", "execPath",
                                                   "platform", "stdout", "stderr"};
  if (llvm::is_contained(properties, name)) {
    sema_.diags_.error(member->loc(), "'process." + name + "' is not a function");
  } else {
    sema_.diags_.error(member->loc(), "'process." + name + "' is not supported yet");
  }
  checkArgsLoosely();

  return call;
}

// process.argv, process.env.NAME, process.exitCode, process.execPath,
// process.platform and process.stdout.isTTY, and errors for the other
// properties of process, Date and performance that are read.
Expr *ProcessChecker::checkProcessMember(MemberExpr *member) {
  llvm::StringRef name = member->name;
  if (isProcessMember(member->object, "env")) {
    // `string | undefined`, as in Node.
    auto *key = sema_.ctx_.make<StringLitExpr>(member->loc(), toUtf16(name));
    key->type = Type::string();
    return builtinCall(member->loc(), member, Builtin::ProcessEnv, {key},
                       Type::optional(Type::string()));
  }

  bool isStdout = isProcessMember(member->object, "stdout");
  if (isStdout || isProcessMember(member->object, "stderr")) {
    std::string stream = isStdout ? "process.stdout" : "process.stderr";
    // A field of the stream of node:stream (sema_stream.cpp); a terminal
    // has none of its own yet.
    if (name != "isTTY" && name != "columns" && name != "rows" &&
        sema_.libFunctions_.lookup("processStdout")) {
      return nullptr;
    }
    if (name == "isTTY") {
      // Whether the stream is a terminal. A boolean, as the types of Node
      // say; Node itself has undefined instead of false.
      auto *fd = sema_.ctx_.make<IntLitExpr>(member->loc(), isStdout ? 1 : 2);
      fd->type = Type::intTy();
      return builtinCall(member->loc(), member, Builtin::ProcessIsTTY, {fd}, Type::boolTy());
    }
    if (name == "write") {
      sema_.diags_.error(member->loc(),
                   "'" + stream + ".write' can only be called: '" + stream + ".write(text)'");
    } else {
      sema_.diags_.error(member->loc(), "'" + stream + "." + name + "' is not supported yet; only " +
                                      "'isTTY', 'write()' and the rest of a Writable of " +
                                      "node:stream are");
    }
    member->type = Type::error();
    return member;
  }

  if (sema_.callChecker().isGlobalName(member->object, "Date")) {
    return sema_.dateChecker().checkDateStaticMember(member); // sema_date.cpp
  }
  if (sema_.callChecker().isGlobalName(member->object, "performance")) {
    if (name == "now") {
      sema_.diags_.error(member->loc(), "'performance.now' can only be called: 'performance.now()'");
    } else {
      sema_.diags_.error(member->loc(),
                   "'performance." + name + "' is not supported yet; only 'performance.now()' is");
    }
    member->type = Type::error();
    return member;
  }

  if (!sema_.callChecker().isGlobalName(member->object, "process")) {
    return nullptr;
  }
  if (name == "stdin" || name == "stdout" || name == "stderr") {
    if (Expr *stream = sema_.streamSources().processStream(member)) {
      return stream;
    }
  }
  if (name == "argv") {
    return builtinCall(member->loc(), member, Builtin::ProcessArgv, {},
                       Type::array(Type::string()));
  }
  if (name == "exitCode") { // undefined until the program sets it
    return builtinCall(member->loc(), member, Builtin::ProcessExitCode, {},
                       Type::optional(Type::intTy()));
  }
  if (name == "execPath") {
    return builtinCall(member->loc(), member, Builtin::ProcessExecPath, {}, Type::string());
  }
  if (name == "platform") {
    return builtinCall(member->loc(), member, Builtin::ProcessPlatform, {}, Type::string());
  }
  // The whole of it, a dictionary (docs/io.md): a key read by name above is
  // the variable itself.
  if (name == "env") {
    return builtinCall(member->loc(), member, Builtin::EnvMap, {},
                       Type::dict(Type::DictKind::Record, Type::string(), Type::string()));
  }
  member->type = Type::error();
  if (name == "exit" || name == "cwd") {
    sema_.diags_.error(member->loc(), "'process." + name + "' can only be called: 'process." + name +
                                    (name == "exit" ? "(code)'" : "()'"));
  } else if (name == "stdout" || name == "stderr") {
    sema_.diags_.error(member->loc(), "'process." + name + "' can only be used as 'process." + name +
                                    ".write(text)' or 'process." + name + ".isTTY'");
  } else {
    sema_.diags_.error(member->loc(), "'process." + name + "' is not supported yet");
  }

  return member;
}

// process.env[name]
Expr *ProcessChecker::checkProcessEnvIndex(IndexExpr *index) {
  if (!isProcessMember(index->object, "env")) {
    return nullptr;
  }
  Expr *key = sema_.argumentChecker().checkArg(index->index, Type::string());

  return builtinCall(index->loc(), index->object, Builtin::ProcessEnv, {key},
                     Type::optional(Type::string()));
}

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
  bool isEnv = isProcessMember(object, "env");
  bool isProcess = sema_.callChecker().isGlobalName(object, "process") && !name.empty();
  bool isStream = isProcessMember(object, "stdout") || isProcessMember(object, "stderr");
  if (!isEnv && !isProcess && !isStream) {
    return nullptr;
  }

  if (isProcess && name == "exitCode" && !assign->op) {
    // The code the program ends with when its code runs to the end.
    Expr *code = checkExitCode(assign->value);
    return builtinCall(assign->loc(), assign->target, Builtin::ProcessSetExitCode, {code},
                       Type::voidTy());
  }
  // A key of process.env is set as one of any Record, through the dictionary,
  // which changes the variable too.
  if (isEnv) {
    return nullptr;
  }
  if (isProcess && name == "exitCode") {
    sema_.diags_.error(assign->loc(), "only '=' can set 'process.exitCode'");
  } else if (isProcess) {
    sema_.diags_.error(assign->target->loc(), "'process." + name + "' cannot be assigned to");
  } else {
    sema_.diags_.error(assign->target->loc(),
                 "'process." + cast<MemberExpr>(object)->name + "' cannot be changed");
  }
  assign->value = sema_.exprChecker().checkExpr(assign->value);
  assign->type = Type::error();

  return assign;
}

} // namespace tz
