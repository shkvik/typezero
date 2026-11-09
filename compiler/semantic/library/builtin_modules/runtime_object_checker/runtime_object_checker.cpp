#include "compiler/semantic/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/program/library/iterator/iterator_method_table/iterator_method_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/callback_checker/callback_checker.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/library/fs/fs_checker/fs_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"

using llvm::dyn_cast;

namespace tz {

RuntimeObjectChecker::RuntimeObjectChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.getPrelude();
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
  this->argumentChecker = &all.getArgumentChecker();
  this->callbackChecker = &all.getCallbackChecker();
}

Expr *RuntimeObjectChecker::checkRuntimeMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  std::string message = ErrorTextRuntimeObject::noMember(name, type->name(), type->runtimeKind());
  this->state.getDiagnostics().error(member->loc(), message);

  return member;
}

Expr *RuntimeObjectChecker::checkRuntimeMethod(CallExpr *call, MemberExpr *member,
                                               const Type *expected) {
  (void)expected;
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }

    return call;
  };
  if (this->callChecker->rejectSpread(call)) {
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
      ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
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
          this->argumentChecker->checkArity(call, 1, promises ? 1 : 3);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(member->loc(), arity.error().message);
        return call;
      }
      Expr *query = this->argumentChecker->checkArg(call->args[0], Type::string());
      if (promises) {
        call->args = {query};
        call->target = BuiltinCall{.builtin = Builtin::RlQuestion};
        call->type = Type::promise(
            Type::result(Type::string(), this->prelude->builtinType("QuestionError")));
        return call;
      }
      if (call->args.size() < 2) {
        std::string message = ErrorTextReadlineMethod::callback();
        this->state.getDiagnostics().error(member->loc(), message);
        return call;
      }
      if (call->args.size() == 3) {
        std::string message = ErrorTextReadlineMethod::options();
        this->state.getDiagnostics().error(call->args[1]->loc(), message);
        return loosely();
      }
      Signature sig;
      sig.params = {Type::string()};
      sig.ret = Type::voidTy();
      sig.voidResult = true;
      sig.asyncVoid = true;
      Expr *callback = this->callbackChecker->checkFunctionArg(call->args[1], sig, 1, "question");
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
      std::string message = ErrorTextReadlineMethod::events(name);
      this->state.getDiagnostics().error(member->loc(), message);
      return loosely();
    }
    break;
  }
  case Type::Runtime::AsyncIterator: {
    const IteratorMethodInfo *method = IteratorMethodTable::find(name);
    std::optional<IteratorMethod> which = std::nullopt;
    if (method != nullptr) {
      which = method->method;
    }
    if (which == IteratorMethod::THROW) {
      std::string message = ErrorTextAsyncIterator::noThrow();
      this->state.getDiagnostics().error(member->loc(), message);
      return loosely();
    }
    if (which == IteratorMethod::NEXT || which == IteratorMethod::RETURN) {
      std::string message = ErrorTextAsyncIterator::notYet(name);
      this->state.getDiagnostics().error(member->loc(), message);
      return loosely();
    }
    break;
  }
  default:
    break;
  }
  std::string message = ErrorTextRuntimeObject::noMember(name, type->name(), type->runtimeKind());
  this->state.getDiagnostics().error(member->loc(), message);

  return loosely();
}

const Type *RuntimeObjectChecker::checkRuntimeTarget(MemberExpr *member, bool compound) {
  (void)compound;
  const Type *type = member->object->type;
  std::string message =
      ErrorTextRuntimeObject::assigned(member->name, type->name(), type->runtimeKind());
  this->state.getDiagnostics().error(member->loc(), message);

  return Type::error();
}

}
