#include "compiler/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/functions/callback_checker/callback_checker.h"
#include "compiler/library/fs/fs_checker/fs_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

const Type *signalType() { return Type::runtimeObject(Type::Runtime::AbortSignal); }

}

RuntimeObjectChecker::RuntimeObjectChecker(Checker &checker) : checker_(checker) {}

Expr *RuntimeObjectChecker::signalOption(Expr *value, llvm::SMLoc loc) {
  const Type *optional = Type::optional(signalType());
  if (!value) {
    auto *none = checker_.ctx_.make<UndefinedExpr>(loc);
    return checker_.argumentChecker().checkArg(none, optional);
  }
  ClassDecl *cls = checker_.libraryClass("AbortSignal");
  if (!cls) {
    return checker_.argumentChecker().checkArg(value, optional);
  }
  Expr *signal = checker_.argumentChecker().checkArg(value, Type::optional(cls->type));
  if (signal->type->isError()) {
    return signal;
  }
  auto *callee = checker_.ctx_.make<UndefinedExpr>(loc);
  callee->type = Type::undefinedTy();

  return checker_.processChecker().builtinCall(loc, callee, Builtin::LibNativeSignal, {signal},
                                               optional);
}

Expr *RuntimeObjectChecker::checkRuntimeMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  checker_.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                           type->name() + "'" + runtimeMembers(type));

  return member;
}

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

Expr *RuntimeObjectChecker::checkRuntimeMethod(CallExpr *call, MemberExpr *member,
                                               const Type *expected) {
  (void)expected;
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }

    return call;
  };
  if (checker_.callChecker().rejectSpread(call)) {
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
      if (!checker_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
        return call;
      }
      call->builtin = Builtin::RlClose;
      call->type = Type::voidTy();
      return call;
    }
    if (name == "question") {
      if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, promises ? 2 : 3)) {
        return call;
      }
      Expr *query = checker_.argumentChecker().checkArg(call->args[0], Type::string());
      if (promises) {
        llvm::StringMap<Expr *> options;
        if (!checker_.fsChecker().nodeOptions(call, 1, "question", {"signal"}, options)) {
          return call;
        }
        call->args = {query, signalOption(options.lookup("signal"), member->loc())};
        call->builtin = Builtin::RlQuestion;
        call->type =
            Type::promise(Type::result(Type::string(), checker_.fsChecker().systemError()));
        return call;
      }
      if (call->args.size() < 2) {
        checker_.diags_.error(member->loc(),
                              "'question' of node:readline takes a callback for the answer, "
                              "'rl.question(query, answer => ...)'; for a Promise, import "
                              "'createInterface' from \"node:readline/promises\"");
        return call;
      }
      if (call->args.size() == 3) {
        checker_.diags_.error(call->args[1]->loc(),
                              "the options of 'question' with a callback are not "
                              "supported yet");
        return loosely();
      }
      Checker::Signature sig;
      sig.params = {Type::string()};
      sig.ret = Type::voidTy();
      sig.voidResult = true;
      sig.asyncVoid = true;
      Expr *callback =
          checker_.callbackChecker().checkFunctionArg(call->args[1], sig, 1, "question");
      if (!callback->type->isFunction()) {
        return call;
      }
      if (auto *fe = dyn_cast<FunctionExpr>(callback)) {
        fe->fn->nonEscaping = false;
      }
      call->args = {query, callback};
      call->builtin = Builtin::RlQuestionCallback;
      call->type = Type::voidTy();
      return call;
    }
    if (llvm::is_contained(events, name)) {
      checker_.diags_.error(member->loc(),
                            "'" + name +
                                "' of readline is not supported yet: its Interface is no "
                                "EventEmitter yet (docs/events.md); read the lines with "
                                "'for await (const line of rl)' or ask with 'question()'");
      return loosely();
    }
    break;
  }
  case Type::Runtime::AsyncIterator:
    if (name == "next" || name == "return" || name == "throw") {
      checker_.diags_.error(member->loc(), "'" + name +
                                               "()' of an async iterator is not supported yet; "
                                               "iterate it with 'for await'");
      return loosely();
    }
    break;
  default:
    break;
  }
  checker_.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                           type->name() + "'" + runtimeMembers(type));

  return loosely();
}

const Type *RuntimeObjectChecker::checkRuntimeTarget(MemberExpr *member, bool compound) {
  (void)compound;
  const Type *type = member->object->type;
  checker_.diags_.error(member->loc(), "property '" + member->name + "' of type '" + type->name() +
                                           "' cannot be assigned to" + runtimeMembers(type));

  return Type::error();
}

}
