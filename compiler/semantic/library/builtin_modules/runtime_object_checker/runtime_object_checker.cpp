#include "compiler/semantic/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/program/library/fs/readline_method_table/readline_method_table.h"
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
  this->prelude = &all.prelude;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->argumentChecker = &all.argumentChecker;
  this->callbackChecker = &all.callbackChecker;
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
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  switch (member->object->type->runtimeKind()) {
    case Type::Runtime::Readline:
    case Type::Runtime::ReadlinePromises:
      return this->checkReadlineMethod(call, member);
    case Type::Runtime::AsyncIterator:
      return this->checkAsyncIteratorMethod(call, member);
    case Type::Runtime::None:
    case Type::Runtime::FileHandle:
      return this->reportNoMember(call, member);
  }

  return this->reportNoMember(call, member);
}

Expr *RuntimeObjectChecker::checkArgsLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return call;
}

Expr *RuntimeObjectChecker::reportNoMember(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  std::string message =
      ErrorTextRuntimeObject::noMember(member->name, type->name(), type->runtimeKind());
  this->state.getDiagnostics().error(member->loc(), message);

  return this->checkArgsLoosely(call);
}

bool RuntimeObjectChecker::hasWrongArity(CallExpr *call, MemberExpr *member, unsigned min,
                                         unsigned max) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, min, max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }

  return arity.isErr();
}

Expr *RuntimeObjectChecker::checkReadlineMethod(CallExpr *call, MemberExpr *member) {
  std::optional<ReadlineMethod> method = ReadlineMethodTable::find(member->name);
  if (!method.has_value()) {
    return this->reportNoMember(call, member);
  }
  switch (*method) {
    case ReadlineMethod::CLOSE:
      return this->checkReadlineClose(call, member);
    case ReadlineMethod::QUESTION:
      return this->checkQuestion(call, member);
    case ReadlineMethod::ON:
    case ReadlineMethod::ONCE:
    case ReadlineMethod::ADD_LISTENER:
    case ReadlineMethod::OFF:
    case ReadlineMethod::REMOVE_LISTENER:
    case ReadlineMethod::REMOVE_ALL_LISTENERS:
    case ReadlineMethod::EMIT:
    case ReadlineMethod::PREPEND_LISTENER:
    case ReadlineMethod::PREPEND_ONCE_LISTENER:
    case ReadlineMethod::LISTENERS:
    case ReadlineMethod::LISTENER_COUNT:
    case ReadlineMethod::ADD_EVENT_LISTENER:
    case ReadlineMethod::REMOVE_EVENT_LISTENER:
    case ReadlineMethod::DISPATCH_EVENT:
      return this->reportReadlineEvents(call, member);
  }

  return this->reportNoMember(call, member);
}

Expr *RuntimeObjectChecker::reportReadlineEvents(CallExpr *call, MemberExpr *member) {
  std::string message = ErrorTextReadlineMethod::events(member->name);
  this->state.getDiagnostics().error(member->loc(), message);

  return this->checkArgsLoosely(call);
}

Expr *RuntimeObjectChecker::checkReadlineClose(CallExpr *call, MemberExpr *member) {
  if (this->hasWrongArity(call, member, 0, 0)) {
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::RlClose};
  call->type = Type::voidTy();

  return call;
}

Expr *RuntimeObjectChecker::checkQuestion(CallExpr *call, MemberExpr *member) {
  bool hasPromises = member->object->type->runtimeKind() == Type::Runtime::ReadlinePromises;
  if (this->hasWrongArity(call, member, 1, hasPromises ? 1 : 3)) {
    return call;
  }
  Expr *query = this->argumentChecker->checkArg(call->args[0], Type::string());
  if (!hasPromises) {
    return this->checkQuestionCallback(call, member, query);
  }
  call->args = {query};
  call->target = BuiltinCall{.builtin = Builtin::RlQuestion};
  const Type *failure = this->prelude->builtinType("QuestionError");
  call->type = Type::promise(Type::result(Type::string(), failure));

  return call;
}

Expr *RuntimeObjectChecker::checkQuestionCallback(CallExpr *call, MemberExpr *member, Expr *query) {
  if (call->args.size() < 2) {
    std::string message = ErrorTextReadlineMethod::callback();
    this->state.getDiagnostics().error(member->loc(), message);
    return call;
  }
  if (call->args.size() == 3) {
    std::string message = ErrorTextReadlineMethod::options();
    this->state.getDiagnostics().error(call->args[1]->loc(), message);
    return this->checkArgsLoosely(call);
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
  auto *fe = dyn_cast<FunctionExpr>(callback);
  if (fe != nullptr) {
    fe->fn->nonEscaping = false;
  }
  call->args = {query, callback};
  call->target = BuiltinCall{.builtin = Builtin::RlQuestionCallback};
  call->type = Type::voidTy();

  return call;
}

Expr *RuntimeObjectChecker::checkAsyncIteratorMethod(CallExpr *call, MemberExpr *member) {
  const IteratorMethodInfo *method = IteratorMethodTable::find(member->name);
  std::optional<IteratorMethod> which =
      method != nullptr ? std::optional<IteratorMethod>(method->method) : std::nullopt;
  if (which == IteratorMethod::THROW) {
    std::string message = ErrorTextAsyncIterator::noThrow();
    this->state.getDiagnostics().error(member->loc(), message);
    return this->checkArgsLoosely(call);
  }
  if (which == IteratorMethod::NEXT || which == IteratorMethod::RETURN) {
    std::string message = ErrorTextAsyncIterator::notYet(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    return this->checkArgsLoosely(call);
  }

  return this->reportNoMember(call, member);
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
