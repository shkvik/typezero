#include "compiler/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/functions/callback_checker/callback_checker.h"
#include "compiler/library/fs/fs_checker/fs_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// The runtime's side of an AbortSignal
// (runtime/library/events/signal/signal.c).
const Type *signalType() { return Type::runtimeObject(Type::Runtime::AbortSignal); }

} // namespace

RuntimeObjectChecker::RuntimeObjectChecker(Sema &sema) : sema_(sema) {}

Expr *RuntimeObjectChecker::signalOption(Expr *value, llvm::SMLoc loc) {
  const Type *optional = Type::optional(signalType());
  if (!value) {
    auto *none = sema_.ctx_.make<UndefinedExpr>(loc);
    return sema_.argumentChecker().checkArg(none, optional);
  }
  // Without the library's AbortSignal, which a file that names AbortSignal
  // or AbortController brings, only undefined can be given.
  ClassDecl *cls = sema_.libraryClass("AbortSignal");
  if (!cls) {
    return sema_.argumentChecker().checkArg(value, optional);
  }
  Expr *signal = sema_.argumentChecker().checkArg(value, Type::optional(cls->type));
  if (signal->type->isError()) {
    return signal;
  }
  auto *callee = sema_.ctx_.make<UndefinedExpr>(loc);
  callee->type = Type::undefinedTy();

  return sema_.processChecker().builtinCall(loc, callee, Builtin::LibNativeSignal, {signal}, optional);
}

Expr *RuntimeObjectChecker::checkRuntimeMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  sema_.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" + type->name() +
                                  "'" + runtimeMembers(type));

  return member;
}

// What the objects of each kind have, for the messages.
std::string RuntimeObjectChecker::runtimeMembers(const Type *type) {
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

Expr *RuntimeObjectChecker::checkRuntimeMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
  (void)expected;
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }

    return call;
  };
  if (sema_.callChecker().rejectSpread(call)) {
    return call;
  }
  static const llvm::StringLiteral events[] = {"on",
                                               "once",
                                               "addListener",
                                               "off",
                                               "removeListener",
                                               "removeAllListeners",
                                               "emit",
                                               "prependListener",
                                               "prependOnceListener",
                                               "listeners",
                                               "listenerCount",
                                               "addEventListener",
                                               "removeEventListener",
                                               "dispatchEvent"};
  switch (type->runtimeKind()) {
  case Type::Runtime::Readline:
  case Type::Runtime::ReadlinePromises: {
    bool promises = type->runtimeKind() == Type::Runtime::ReadlinePromises;
    if (name == "close") {
      if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
        return call;
      }
      call->builtin = Builtin::RlClose;
      call->type = Type::voidTy();
      return call;
    }
    if (name == "question") {
      if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, promises ? 2 : 3)) {
        return call;
      }
      Expr *query = sema_.argumentChecker().checkArg(call->args[0], Type::string());
      if (promises) {
        llvm::StringMap<Expr *> options;
        if (!sema_.fsChecker().nodeOptions(call, 1, "question", {"signal"}, options)) {
          return call;
        }
        call->args = {query, signalOption(options.lookup("signal"), member->loc())};
        call->builtin = Builtin::RlQuestion;
        call->type = Type::promise(Type::string());
        return call;
      }
      // question(query, callback), or with options between them.
      if (call->args.size() < 2) {
        sema_.diags_.error(member->loc(), "'question' of node:readline takes a callback for the answer, "
                                    "'rl.question(query, answer => ...)'; for a Promise, import "
                                    "'createInterface' from \"node:readline/promises\"");
        return call;
      }
      if (call->args.size() == 3) {
        sema_.diags_.error(call->args[1]->loc(), "the options of 'question' with a callback are not "
                                           "supported yet");
        return loosely();
      }
      Sema::Signature sig;
      sig.params = {Type::string()};
      sig.ret = Type::voidTy();
      sig.voidResult = true;
      sig.asyncVoid = true;
      Expr *callback = sema_.callbackChecker().checkFunctionArg(call->args[1], sig, 1, "question");
      if (!callback->type->isFunction()) {
        return call;
      }
      if (auto *fe = dyn_cast<FunctionExpr>(callback)) {
        fe->fn->nonEscaping = false; // it runs later
      }
      call->args = {query, callback};
      call->builtin = Builtin::RlQuestionCallback;
      call->type = Type::voidTy();
      return call;
    }
    if (llvm::is_contained(events, name)) {
      sema_.diags_.error(member->loc(), "'" + name +
                                      "' of readline is not supported yet: its Interface is no "
                                      "EventEmitter yet (docs/events.md); read the lines with "
                                      "'for await (const line of rl)' or ask with 'question()'");
      return loosely();
    }
    break;
  }
  case Type::Runtime::AsyncIterator:
    if (name == "next" || name == "return" || name == "throw") {
      sema_.diags_.error(member->loc(), "'" + name +
                                      "()' of an async iterator is not supported yet; "
                                      "iterate it with 'for await'");
      return loosely();
    }
    break;
  default:
    break;
  }
  sema_.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" + type->name() +
                                  "'" + runtimeMembers(type));

  return loosely();
}

const Type *RuntimeObjectChecker::checkRuntimeTarget(MemberExpr *member, bool compound) {
  (void)compound;
  const Type *type = member->object->type;
  sema_.diags_.error(member->loc(), "property '" + member->name + "' of type '" + type->name() +
                                  "' cannot be assigned to" + runtimeMembers(type));

  return Type::error();
}

} // namespace tz
