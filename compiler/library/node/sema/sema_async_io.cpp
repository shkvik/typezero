// Waiting for I/O (docs/async.md, step 2): node:fs/promises,
// node:timers/promises, node:readline over stdin, and the signals that
// cancel them.
//
// The functions of node:fs/promises are checked as those of node:fs that end
// in Sync (sema_node.cpp), with their own options, and give a Promise of what
// those give. The objects of the runtime they make (the Interface of
// node:readline) are types of their own (Type::Runtime) that the program
// reaches only through their methods and properties, which become built-in
// calls. AbortController and AbortSignal are classes of tz's library on
// EventTarget (src/lib/web.tz); what the runtime gets of a signal is its
// side of it (runtime/signal.c), also a type of the runtime.

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// The runtime's side of an AbortSignal (runtime/signal.c).
const Type *signalType() { return Type::runtimeObject(Type::Runtime::AbortSignal); }

} // namespace

Expr *Sema::signalOption(Expr *value, llvm::SMLoc loc) {
  const Type *optional = Type::optional(signalType());
  if (!value) {
    auto *none = ctx_.make<UndefinedExpr>(loc);
    return checkArg(none, optional);
  }
  // Without the library's AbortSignal, which a file that names AbortSignal
  // or AbortController brings, only undefined can be given.
  ClassDecl *cls = libraryClass("AbortSignal");
  if (!cls)
    return checkArg(value, optional);
  Expr *signal = checkArg(value, Type::optional(cls->type));
  if (signal->type->isError())
    return signal;
  auto *callee = ctx_.make<UndefinedExpr>(loc);
  callee->type = Type::undefinedTy();
  return builtinCall(loc, callee, Builtin::LibNativeSignal, {signal}, optional);
}

//===----------------------------------------------------------------------===//
// node:fs/promises
//===----------------------------------------------------------------------===//

Expr *Sema::checkFsPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  file_->usesAsync = true;
  call->type = Type::error();
  if (fn == "constants") {
    diags_.error(loc, "'constants' of 'node:fs/promises' is not a function");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  if (fn == "open") {
    diags_.error(loc, "'open' of 'node:fs/promises' is not supported yet: there is no FileHandle; "
                      "use 'readFile', 'writeFile' or 'appendFile', which read and write "
                      "Buffers too");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  {
    llvm::SaveAndRestore<bool> async(fsAsync_, true);
    checkFsCall(call, fn, loc);
  }
  if (call->type->isError())
    return call;
  static const std::pair<Builtin, Builtin> waiting[] = {
      {Builtin::FsReadFile, Builtin::FspReadFile}, {Builtin::FsWriteFile, Builtin::FspWriteFile},
      {Builtin::FsAccess, Builtin::FspAccess},     {Builtin::FsChmod, Builtin::FspChmod},
      {Builtin::FsTruncate, Builtin::FspTruncate}, {Builtin::FsStat, Builtin::FspStat},
      {Builtin::FsReaddir, Builtin::FspReaddir},   {Builtin::FsMkdir, Builtin::FspMkdir},
      {Builtin::FsMkdtemp, Builtin::FspMkdtemp},   {Builtin::FsRmdir, Builtin::FspRmdir},
      {Builtin::FsUnlink, Builtin::FspUnlink},     {Builtin::FsRm, Builtin::FspRm},
      {Builtin::FsRename, Builtin::FspRename},     {Builtin::FsCopyFile, Builtin::FspCopyFile},
      {Builtin::FsRealpath, Builtin::FspRealpath}};
  for (const auto &[sync, async] : waiting)
    if (call->builtin == sync) {
      call->builtin = async;
      call->type = Type::promise(call->type);
      return call;
    }
  diags_.error(loc, "'" + fn + "' of 'node:fs/promises' is not supported yet");
  call->type = Type::error();
  return call;
}

//===----------------------------------------------------------------------===//
// node:timers/promises
//===----------------------------------------------------------------------===//

// setTimeout(delay?, value?, options?) and setImmediate(value?, options?): a
// Promise of the value (void without one) once the time is up;
// setInterval(delay?, value?, options?): an async iterator that gives the
// value each time. The options are `signal` and `ref`.
Expr *Sema::checkTimersPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                                    const Type *expected) {
  file_->usesAsync = true;
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  };
  if (fn == "scheduler") {
    diags_.error(loc, "'scheduler' is used through its functions: 'scheduler.wait(ms)' or "
                      "'scheduler.yield()'");
    return loosely();
  }
  if (rejectSpread(call))
    return call;
  bool immediate = fn == "setImmediate";
  bool interval = fn == "setInterval";
  size_t valueAt = immediate ? 0 : 1;
  if (!checkArity(call, loc, 0, unsigned(valueAt + 2)))
    return call;
  std::vector<Expr *> args;
  if (!immediate) {
    if (call->args.empty() || isa<UndefinedExpr>(call->args[0])) {
      auto *one = ctx_.make<FloatLitExpr>(loc, 1.0);
      one->type = Type::doubleTy();
      args.push_back(one);
    } else {
      args.push_back(checkArg(call->args[0], Type::doubleTy()));
    }
  }
  // The value, whose type the Promise takes.
  const Type *value = Type::voidTy();
  Expr *given = valueAt < call->args.size() ? call->args[valueAt] : nullptr;
  if (given && !isa<UndefinedExpr>(given)) {
    const Type *wanted = nullptr;
    if (expected && expected->nonOptional()->isPromise())
      wanted = expected->nonOptional()->promised();
    if (wanted && (wanted->isVoid() || wanted->isNever()))
      wanted = nullptr;
    given = checkExpr(given, wanted);
    value = given->type;
    if (value->isError())
      return call;
    if (value->isVoid()) {
      diags_.error(given->loc(), "a value of type 'void' cannot be given to '" + fn + "'");
      return call;
    }
    if (value->isUndefined()) {
      value = Type::voidTy();
    } else {
      if (value->isLiteral())
        value = value->literalBase();
      given = coerce(given, value);
      args.push_back(given);
    }
  } else if (given) {
    checkExpr(given);
  }
  llvm::StringMap<Expr *> options;
  if (!nodeOptions(call, valueAt + 1, fn, {"signal", "ref"}, options))
    return call;
  args.push_back(signalOption(options.lookup("signal"), loc));
  if (Expr *ref = options.lookup("ref")) {
    args.push_back(checkArg(ref, Type::boolTy()));
  } else {
    auto *yes = ctx_.make<BoolLitExpr>(loc, true);
    yes->type = Type::boolTy();
    args.push_back(yes);
  }
  call->args = std::move(args);
  call->spread.clear();
  if (interval) {
    call->builtin = Builtin::TimersSetInterval;
    call->type = Type::asyncIterator(value->isVoid() ? Type::undefinedTy() : value);
    return call;
  }
  call->builtin = immediate ? Builtin::TimersSetImmediate : Builtin::TimersSetTimeout;
  call->type = Type::promise(value);
  return call;
}

// scheduler.wait(delay, options?) is setTimeout(delay, undefined, options),
// scheduler.yield() is setImmediate().
Expr *Sema::checkSchedulerCall(CallExpr *call, MemberExpr *member) {
  file_->usesAsync = true;
  call->type = Type::error();
  if (member->name == "wait") {
    if (rejectSpread(call) || !checkArity(call, member->loc(), 1, 2))
      return call;
    Expr *options = call->args.size() > 1 ? call->args[1] : nullptr;
    auto *none = ctx_.make<UndefinedExpr>(member->loc());
    call->args = {call->args[0], none};
    if (options)
      call->args.push_back(options);
    return checkTimersPromisesCall(call, "setTimeout", member->loc(), nullptr);
  }
  if (member->name == "yield") {
    if (rejectSpread(call) || !checkArity(call, member->loc(), 0, 0))
      return call;
    return checkTimersPromisesCall(call, "setImmediate", member->loc(), nullptr);
  }
  diags_.error(member->loc(), "'scheduler." + member->name +
                                  "' does not exist; scheduler has 'wait' and 'yield'");
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  return call;
}

//===----------------------------------------------------------------------===//
// node:readline
//===----------------------------------------------------------------------===//

// createInterface({ input: process.stdin, output: process.stdout }): lines
// of stdin, which `for await` gives one by one; questions are written to the
// output, if there is one. The options are written in the call: input can
// only be stdin.
Expr *Sema::checkReadlineCall(CallExpr *call, NodeModule module, llvm::StringRef fn,
                              llvm::SMLoc loc) {
  file_->usesAsync = true;
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args)
      if (!isProcessMember(arg, "stdin") && !isProcessMember(arg, "stdout") &&
          !isProcessMember(arg, "stderr"))
        arg = checkExpr(arg);
    return call;
  };
  if (fn != "createInterface") {
    const char *name =
        module == NodeModule::Readline ? "node:readline" : "node:readline/promises";
    diags_.error(loc, "'" + fn + "' of '" + name +
                          "' can only be used as a type; call 'createInterface'");
    return loosely();
  }
  if (rejectSpread(call) || !checkArity(call, loc, 1, 2))
    return call;
  Expr *input = nullptr, *output = nullptr, *signal = nullptr;
  if (isa<ObjectLitExpr>(call->args[0])) {
    llvm::StringMap<Expr *> options;
    if (!nodeOptions(call, 0, fn,
                     {"input", "output", "terminal", "crlfDelay", "signal", "historySize",
                      "removeHistoryDuplicates", "prompt", "tabSize", "completer",
                      "escapeCodeTimeout", "history"},
                     options))
      return call;
    input = options.lookup("input");
    output = options.lookup("output");
    signal = options.lookup("signal");
    // What concerns a terminal of readline's own (raw mode, history, a
    // completer) tz leaves to the console, which edits the line.
    for (llvm::StringRef ignored : {"terminal", "crlfDelay", "historySize",
                                    "removeHistoryDuplicates", "tabSize", "escapeCodeTimeout"})
      if (Expr *value = options.lookup(ignored))
        checkExpr(value);
    for (llvm::StringRef unsupported : {"prompt", "completer", "history"})
      if (Expr *value = options.lookup(unsupported)) {
        diags_.error(value->loc(), "the option '" + unsupported.str() +
                                       "' of 'createInterface' is not supported yet");
        return call;
      }
    if (!input) {
      diags_.error(call->args[0]->loc(), "'createInterface' needs the input: '{ input: "
                                         "process.stdin }'");
      return call;
    }
  } else {
    input = call->args[0];
    output = call->args.size() > 1 ? call->args[1] : nullptr;
  }
  if (!isProcessMember(input, "stdin")) {
    diags_.error(input->loc(), "readline reads only stdin yet: the input is 'process.stdin'; for "
                               "the lines of a file, 'readFile' it and split them");
    return call;
  }
  int written = 0;
  if (output && !isa<UndefinedExpr>(output)) {
    if (isProcessMember(output, "stdout"))
      written = 1;
    else if (isProcessMember(output, "stderr"))
      written = 2;
    else {
      diags_.error(output->loc(), "the output of readline is 'process.stdout' or "
                                  "'process.stderr'");
      return call;
    }
  }
  auto *where = ctx_.make<IntLitExpr>(loc, uint64_t(written));
  where->type = Type::intTy();
  call->args = {where, signalOption(signal, loc)};
  call->builtin = Builtin::RlCreate;
  call->type = Type::runtimeObject(module == NodeModule::Readline
                                       ? Type::Runtime::Readline
                                       : Type::Runtime::ReadlinePromises);
  return call;
}

//===----------------------------------------------------------------------===//
// Properties and methods of the objects of the runtime
//===----------------------------------------------------------------------===//

Expr *Sema::checkRuntimeMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  diags_.error(member->loc(), "property '" + name + "' does not exist on type '" + type->name() +
                                  "'" + runtimeMembers(type));
  return member;
}

// What the objects of each kind have, for the messages.
std::string Sema::runtimeMembers(const Type *type) {
  switch (type->runtimeKind()) {
  case Type::Runtime::Readline:
  case Type::Runtime::ReadlinePromises:
    return "; it has 'question()' and 'close()', and 'for await (const line of rl)' gives the "
           "lines";
  case Type::Runtime::AsyncIterator:
    return "; iterate it with 'for await'";
  default:
    return "";
  }
}

Expr *Sema::checkRuntimeMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
  (void)expected;
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  };
  if (rejectSpread(call))
    return call;
  static const llvm::StringLiteral events[] = {
      "on",           "once",          "addListener",   "off",
      "removeListener", "removeAllListeners", "emit", "prependListener",
      "prependOnceListener", "listeners", "listenerCount", "addEventListener",
      "removeEventListener", "dispatchEvent"};
  switch (type->runtimeKind()) {
  case Type::Runtime::Readline:
  case Type::Runtime::ReadlinePromises: {
    bool promises = type->runtimeKind() == Type::Runtime::ReadlinePromises;
    if (name == "close") {
      if (!checkArity(call, member->loc(), 0, 0))
        return call;
      call->builtin = Builtin::RlClose;
      call->type = Type::voidTy();
      return call;
    }
    if (name == "question") {
      if (!checkArity(call, member->loc(), 1, promises ? 2 : 3))
        return call;
      Expr *query = checkArg(call->args[0], Type::string());
      if (promises) {
        llvm::StringMap<Expr *> options;
        if (!nodeOptions(call, 1, "question", {"signal"}, options))
          return call;
        call->args = {query, signalOption(options.lookup("signal"), member->loc())};
        call->builtin = Builtin::RlQuestion;
        call->type = Type::promise(Type::string());
        return call;
      }
      // question(query, callback), or with options between them.
      if (call->args.size() < 2) {
        diags_.error(member->loc(), "'question' of node:readline takes a callback for the answer, "
                                    "'rl.question(query, answer => ...)'; for a Promise, import "
                                    "'createInterface' from \"node:readline/promises\"");
        return call;
      }
      if (call->args.size() == 3) {
        diags_.error(call->args[1]->loc(), "the options of 'question' with a callback are not "
                                           "supported yet");
        return loosely();
      }
      Signature sig;
      sig.params = {Type::string()};
      sig.ret = Type::voidTy();
      sig.voidResult = true;
      sig.asyncVoid = true;
      Expr *callback = checkFunctionArg(call->args[1], sig, 1, "question");
      if (!callback->type->isFunction())
        return call;
      if (auto *fe = dyn_cast<FunctionExpr>(callback))
        fe->fn->nonEscaping = false; // it runs later
      call->args = {query, callback};
      call->builtin = Builtin::RlQuestionCallback;
      call->type = Type::voidTy();
      return call;
    }
    if (llvm::is_contained(events, name)) {
      diags_.error(member->loc(), "'" + name +
                                      "' of readline is not supported yet: its Interface is no "
                                      "EventEmitter yet (docs/events.md); read the lines with "
                                      "'for await (const line of rl)' or ask with 'question()'");
      return loosely();
    }
    break;
  }
  case Type::Runtime::AsyncIterator:
    if (name == "next" || name == "return" || name == "throw") {
      diags_.error(member->loc(), "'" + name + "()' of an async iterator is not supported yet; "
                                               "iterate it with 'for await'");
      return loosely();
    }
    break;
  default:
    break;
  }
  diags_.error(member->loc(), "property '" + name + "' does not exist on type '" + type->name() +
                                  "'" + runtimeMembers(type));
  return loosely();
}

const Type *Sema::checkRuntimeTarget(MemberExpr *member, bool compound) {
  (void)compound;
  const Type *type = member->object->type;
  diags_.error(member->loc(), "property '" + member->name + "' of type '" + type->name() +
                                  "' cannot be assigned to" + runtimeMembers(type));
  return Type::error();
}

} // namespace tz
