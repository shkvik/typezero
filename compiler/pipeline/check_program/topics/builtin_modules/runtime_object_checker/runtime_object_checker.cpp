#include "compiler/pipeline/check_program/topics/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/pipeline/check_program/declare/prelude/prelude.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/fs/fs_checker/fs_checker.h"
#include "compiler/pipeline/check_program/topics/functions/callback_checker/callback_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"

using llvm::dyn_cast;

namespace tz {

RuntimeObjectChecker::RuntimeObjectChecker(CheckState &owner)
    : state(owner) {}

Expr *RuntimeObjectChecker::checkRuntimeMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  this->state.getDiagnostics().error(
      member->loc(), "property '" + name + "' does not exist on type '" + type->name() + "'" +
                         RuntimeObjectChecker::runtimeMembers(type));

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
      arg = ExprChecker(this->state).checkExpr(arg);
    }

    return call;
  };
  if (CallChecker(this->state).rejectSpread(call)) {
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
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 0, 0);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(member->loc(), arity.error().message);
        return call;
      }
      call->target = BuiltinCall{.builtin = Builtin::RlClose};
      call->type = Type::voidTy();
      return call;
    }
    if (name == "question") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 1, promises ? 1 : 3);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(member->loc(), arity.error().message);
        return call;
      }
      Expr *query = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
      if (promises) {
        call->args = {query};
        call->target = BuiltinCall{.builtin = Builtin::RlQuestion};
        call->type = Type::promise(
            Type::result(Type::string(), Prelude(this->state).builtinType("QuestionError")));
        return call;
      }
      if (call->args.size() < 2) {
        this->state.getDiagnostics().error(
            member->loc(), "'question' of node:readline takes a callback for the answer, "
                           "'rl.question(query, answer => ...)'; for a Promise, import "
                           "'createInterface' from \"node:readline/promises\"");
        return call;
      }
      if (call->args.size() == 3) {
        this->state.getDiagnostics().error(call->args[1]->loc(),
                                           "the options of 'question' with a callback are not "
                                           "supported yet");
        return loosely();
      }
      Signature sig;
      sig.params = {Type::string()};
      sig.ret = Type::voidTy();
      sig.voidResult = true;
      sig.asyncVoid = true;
      Expr *callback =
          CallbackChecker(this->state).checkFunctionArg(call->args[1], sig, 1, "question");
      if (!callback->type->isFunction()) {
        return call;
      }
      if (auto *fe = dyn_cast<FunctionExpr>(callback)) {
        fe->fn->nonEscaping = false;
      }
      call->args = {query, callback};
      call->target = BuiltinCall{.builtin = Builtin::RlQuestionCallback};
      call->type = Type::voidTy();
      return call;
    }
    if (llvm::is_contained(events, name)) {
      this->state.getDiagnostics().error(
          member->loc(), "'" + name +
                             "' of readline is not supported yet: its Interface is no "
                             "EventEmitter yet (docs/events.md); read the lines with "
                             "'for await (const line of rl)' or ask with 'question()'");
      return loosely();
    }
    break;
  }
  case Type::Runtime::AsyncIterator:
    if (name == "throw") {
      this->state.getDiagnostics().error(member->loc(),
                                         "an iterator has no 'throw()' in tz: an error is a "
                                         "value, not something thrown into a generator");
      return loosely();
    }
    if (name == "next" || name == "return") {
      this->state.getDiagnostics().error(member->loc(),
                                         "'" + name +
                                             "()' of an async iterator is not supported yet; "
                                             "iterate it with 'for await'");
      return loosely();
    }
    break;
  default:
    break;
  }
  this->state.getDiagnostics().error(
      member->loc(), "property '" + name + "' does not exist on type '" + type->name() + "'" +
                         RuntimeObjectChecker::runtimeMembers(type));

  return loosely();
}

const Type *RuntimeObjectChecker::checkRuntimeTarget(MemberExpr *member, bool compound) {
  (void)compound;
  const Type *type = member->object->type;
  this->state.getDiagnostics().error(member->loc(), "property '" + member->name + "' of type '" +
                                                        type->name() + "' cannot be assigned to" +
                                                        RuntimeObjectChecker::runtimeMembers(type));

  return Type::error();
}

}
