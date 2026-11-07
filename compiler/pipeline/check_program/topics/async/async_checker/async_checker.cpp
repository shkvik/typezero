#include "compiler/pipeline/check_program/topics/async/async_checker/async_checker.h"
#include "compiler/pipeline/check_program/flow/flow_checker/flow_checker.h"
#include "compiler/pipeline/check_program/flow/loop_bounds/loop_bounds.h"
#include "compiler/pipeline/check_program/flow/loop_scanner/loop_scanner.h"
#include "compiler/pipeline/check_program/generics/generic_checker/generic_checker.h"
#include "compiler/pipeline/check_program/generics/generic_widening/generic_widening.h"
#include "compiler/pipeline/check_program/scopes/scopes.h"
#include "compiler/pipeline/check_program/shared_helpers/shared_helpers.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/functions/function_checker/function_checker_errors.h"
#include "compiler/pipeline/check_program/topics/iterators/generator_checker/generator_checker.h"
#include "compiler/pipeline/check_program/topics/iterators/iterable_checker/iterable_checker.h"
#include "compiler/pipeline/check_program/topics/iterators/iterator_methods/iterator_methods.h"
#include "compiler/pipeline/check_program/topics/objects/dict_checker/dict_checker.h"
#include "compiler/pipeline/check_program/topics/regex/match_checker/match_checker.h"
#include "compiler/pipeline/check_program/topics/stmt/loop_checker/loop_checker.h"
#include "compiler/pipeline/check_program/types/literal_types/literal_types.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

const Type *literalType(const Expr *value);

const Type *AsyncChecker::awaited(const Type *type) {
  return type->isPromise() ? type->promised() : type;
}

std::string AsyncChecker::statementText(llvm::SMLoc at) {
  if (!at.isValid()) {
    return "";
  }
  const char *start = at.getPointer();
  const char *end = start;
  int depth = 0;
  for (; *end && *end != '\n' && *end != '\r'; ++end) {
    if (*end == '(' || *end == '[' || *end == '{') {
      ++depth;
    } else if (*end == ')' || *end == ']' || *end == '}') {
      --depth;
    } else if (depth == 0 && (*end == ';' || (end[0] == '/' && end[1] == '/'))) {
      break;
    }
  }
  llvm::StringRef text = llvm::StringRef(start, size_t(end - start)).rtrim();

  return depth == 0 && text.size() <= 60 ? text.str() : "";
}

AsyncChecker::AsyncChecker(CheckState &owner)
    : state(owner) {}

AsyncChecker::ResultPromiseTypeRef AsyncChecker::resolvePromiseType(const TypeRef &ref) {
  if (ref.name != "Promise" && ref.name != "PromiseLike") {
    return err(AsyncCheckerResolvePromiseTypeError{
        .code = AsyncCheckerResolvePromiseTypeErrorCode::NOT_A_PROMISE,
        .message = "",
    });
  }
  if (ref.args.size() != 1) {
    std::string message = ErrorTextPromiseType::typeArgs(ref.name);

    return err(AsyncCheckerResolvePromiseTypeError{
        .code = AsyncCheckerResolvePromiseTypeErrorCode::WRONG_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  const Type *value = TypeResolver(this->state).resolveType(ref.args[0], true);
  const Type *promise = Type::promise(AsyncChecker::awaited(value));

  return ok(promise);
}

AsyncChecker::ResultReturnsPromise AsyncChecker::promiseDeclared(const FunctionDecl *fn,
                                                                 const Type *declared) {
  bool plain = fn->isAsync && !fn->isGenerator && !declared->isError() && !declared->isPromise();
  if (plain) {
    std::string message = ErrorTextPromiseType::notPromise(declared->name());

    return err(AsyncCheckerPromiseDeclaredError{
        .code = AsyncCheckerPromiseDeclaredErrorCode::NOT_A_PROMISE,
        .message = message,
    });
  }

  return ok(declared);
}

const Type *AsyncChecker::asyncReturnType(const FunctionDecl *fn, const Type *declared) {
  ResultReturnsPromise promise = AsyncChecker::promiseDeclared(fn, declared);
  if (promise.isOk()) {
    return declared;
  }
  this->state.getDiagnostics().error(fn->returnType->loc, promise.error().message);

  return Type::promise(declared);
}

AsyncChecker::ResultReturnsOneType AsyncChecker::returnsJoin(const Type *type, const Type *t,
                                                             const FuncSymbol *fn) {
  const Type *common = type ? TypeRelations::unify(type, t) : t;
  if (!common || common->isError()) {
    std::string name = ErrorTextFunctionName::quoted(fn->name);
    std::string message = ErrorTextAsyncReturns::differ(name, t->name(), type->name());

    return err(AsyncCheckerReturnsJoinError{
        .code = AsyncCheckerReturnsJoinErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(common);
}

const Type *AsyncChecker::inferAsyncType(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                                         bool fallsOff, bool report) {
  const Type *type = nullptr;
  bool undefined = fallsOff;
  bool unknown = false;
  for (ReturnStmt *ret : returns) {
    if (!ret->value) {
      undefined = true;
      continue;
    }
    const Type *t = AsyncChecker::awaited(ret->value->type);
    if (t->isError()) {
      unknown = true;
      continue;
    }
    if (t->isUndefined() || t->isVoid()) {
      undefined = true;
      continue;
    }
    ResultReturnsOneType joined = AsyncChecker::returnsJoin(type, t, fn);
    if (joined.isErr()) {
      if (report) {
        this->state.getDiagnostics().error(ret->value->loc(), joined.error().message);
      }
      return Type::error();
    }
    type = joined.value();
  }
  if (!type) {
    if (unknown) {
      return report ? Type::error() : nullptr;
    }
    return Type::promise(Type::voidTy());
  }
  if (undefined) {
    type = Type::optional(type);
  }

  return Type::promise(type);
}

AsyncChecker::ResultPromiseMatches
AsyncChecker::promiseMatches(const Type *given, const Type *value, const FuncSymbol *fn) {
  if (given != value && !given->isNever()) {
    std::string name = ErrorTextFunctionName::quoted(fn->name);
    std::string message = ErrorTextAsyncReturns::promise(given->name(), name, value->name());

    return err(AsyncCheckerPromiseMatchesError{
        .code = AsyncCheckerPromiseMatchesErrorCode::OTHER_PROMISE_TYPE,
        .message = message,
    });
  }

  return ok(given);
}

void AsyncChecker::finishAsyncReturns(FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns) {
  const Type *value = fn->ret->isPromise() ? fn->ret->promised() : nullptr;
  if (!value || value->isError()) {
    return;
  }
  for (ReturnStmt *ret : returns) {
    if (!ret->value || ret->value->type->isError()) {
      continue;
    }
    const Type *t = ret->value->type;
    if (t->isPromise()) {
      ResultPromiseMatches matches = AsyncChecker::promiseMatches(t->promised(), value, fn);
      if (matches.isErr()) {
        this->state.getDiagnostics().error(ret->value->loc(), matches.error().message);
      }
      continue;
    }
    if (!value->isVoid()) {
      ret->value = TypeRelations(this->state).convert(ret->value, value);
    }
  }
}

AsyncChecker::ResultInAsyncContext AsyncChecker::asyncContext(bool loop) const {
  FuncSymbol *fn = this->state.getFunctionStack().getCurrentFunction();
  bool plain = fn && !(fn->decl && fn->decl->isAsync);
  if (plain && loop) {
    std::string message = ErrorTextAsyncContext::forAwait();

    return err(AsyncCheckerAsyncContextError{
        .code = AsyncCheckerAsyncContextErrorCode::FOR_AWAIT_NOT_ASYNC,
        .message = message,
    });
  }
  if (plain) {
    FunctionDecl *decl = fn->decl;
    std::string hint = decl && decl->isArrow()       ? ErrorTextAsyncContext::arrowHint()
                       : decl && decl->cls()         ? ErrorTextAsyncContext::methodHint(decl->name)
                       : decl && !decl->name.empty() ? ErrorTextAsyncContext::namedHint(decl->name)
                                                     : ErrorTextAsyncContext::plainHint();
    std::string message = ErrorTextAsyncContext::await(hint);

    return err(AsyncCheckerAsyncContextError{
        .code = AsyncCheckerAsyncContextErrorCode::AWAIT_NOT_ASYNC,
        .message = message,
    });
  }

  return ok(fn);
}

AsyncChecker::ResultAwaitedOneType AsyncChecker::awaitable(const Type *type) {
  const Type *result = AsyncChecker::awaitedType(type);
  if (!result) {
    std::string message = ErrorTextAwaited::mixed(type->name());

    return err(AsyncCheckerAwaitableError{
        .code = AsyncCheckerAwaitableErrorCode::MIXED_PROMISE,
        .message = message,
    });
  }

  return ok(result);
}

Expr *AsyncChecker::checkAwait(AwaitExpr *await, const Type *expected) {
  await->type = Type::error();
  this->state.getProgramUse().markUsesAsync();
  ResultInAsyncContext context = this->asyncContext(false);
  if (context.isErr()) {
    this->state.getDiagnostics().error(await->loc(), context.error().message);
  } else if (!context.value()) {
    this->state.getProgramUse().markTopLevelAwait();
  }
  const Type *hint = nullptr;
  if (expected && !expected->isError() && !expected->isVoid() &&
      (isa<NewExpr>(await->value) || isa<CallExpr>(await->value))) {
    hint = Type::promise(expected);
  }
  await->value = ExprChecker(this->state).checkExpr(await->value, hint);
  const Type *type = await->value->type;
  FlowChecker(this->state).calledFunction();
  if (type->isError()) {
    return await;
  }
  ResultAwaitedOneType result = AsyncChecker::awaitable(type);
  if (result.isErr()) {
    this->state.getDiagnostics().error(await->value->loc(), result.error().message);
    return await;
  }
  await->type = result.value();

  return await;
}

const Type *AsyncChecker::awaitedType(const Type *type) {
  if (type->isPromise()) {
    return type->promised();
  }
  if (type->isOptional()) {
    const Type *inner = AsyncChecker::awaitedType(type->inner());
    if (!inner || inner->isVoid()) {
      return inner;
    }
    return Type::optional(inner);
  }
  if (!type->isUnion() ||
      llvm::none_of(type->members(), [](const Type *m) { return m->isPromise(); })) {
    return type;
  }
  const Type *result = nullptr;
  for (const Type *m : type->members()) {
    const Type *t = m->isPromise() ? m->promised() : m;
    result = result ? LiteralTypes::unionUnify(result, t) : t;
    if (!result || result->isError()) {
      return nullptr;
    }
  }

  return result;
}

AsyncChecker::ResultAwaitedElement AsyncChecker::awaitedElement(const Type *type) const {
  const Type *element = GeneratorChecker(this->state).asyncIteratedType(type);
  bool optional = !element && type->isOptional() &&
                  GeneratorChecker(this->state).asyncIteratedType(type->inner()) != nullptr;
  if (optional) {
    std::string message = ErrorTextAwaitedElement::undefined(type->name());

    return err(AsyncCheckerAwaitedElementError{
        .code = AsyncCheckerAwaitedElementErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!element && type->isRecord()) {
    std::string message = ErrorTextAwaitedElement::record();

    return err(AsyncCheckerAwaitedElementError{
        .code = AsyncCheckerAwaitedElementErrorCode::RECORD_NOT_ITERABLE,
        .message = message,
    });
  }
  if (!element && !type->isError()) {
    std::string message = ErrorTextAwaitedElement::notAsync(type->name());

    return err(AsyncCheckerAwaitedElementError{
        .code = AsyncCheckerAwaitedElementErrorCode::NOT_ASYNC_ITERABLE,
        .message = message,
    });
  }
  if (!element) {
    return err(AsyncCheckerAwaitedElementError{
        .code = AsyncCheckerAwaitedElementErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }

  return ok(element);
}

AsyncChecker::ResultBindingUntyped AsyncChecker::bindingType(const VarDeclarator &binding) {
  if (binding.typeAnn) {
    std::string message = ErrorTextBindingType::annotated();

    return err(AsyncCheckerBindingTypeError{
        .code = AsyncCheckerBindingTypeErrorCode::HAS_TYPE_ANNOTATION,
        .message = message,
    });
  }

  return ok(&binding);
}

AsyncChecker::ResultBindingPattern AsyncChecker::bindingShape(const VarDeclarator &binding,
                                                              const Type *element) {
  bool fits = !binding.destructured() || binding.objectPattern() || element->isArray() ||
              element->isTuple() || element->isMatch() || element->isError();
  if (!fits) {
    std::string message = ErrorTextBindingShape::notArray(element->name());

    return err(AsyncCheckerBindingShapeError{
        .code = AsyncCheckerBindingShapeErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(element);
}

void AsyncChecker::checkForAwait(ForOfStmt *stmt) {
  this->state.getProgramUse().markUsesAsync();
  ResultInAsyncContext context = this->asyncContext(true);
  if (context.isErr()) {
    this->state.getDiagnostics().error(stmt->loc(), context.error().message);
  } else if (!context.value()) {
    this->state.getProgramUse().markTopLevelAwait();
  }
  IteratorMethods(this->state).markFused(stmt->iterable);
  stmt->iterable =
      GenericChecker(this->state).apparent(ExprChecker(this->state).checkExpr(stmt->iterable));
  stmt->iterable = GenericWidening(this->state).joined(stmt->iterable);
  if (Expr *iterator = IterableChecker(this->state).asyncUserIterator(stmt->iterable)) {
    stmt->iterable = iterator;
  } else {
    stmt->iterable = DictChecker(this->state).iterableOf(stmt->iterable);
  }
  if (stmt->iterable->type->isMatch()) {
    stmt->iterable = MatchChecker(this->state).matchItems(stmt->iterable);
  }
  const Type *type = stmt->iterable->type;
  const Type *element = Type::error();
  ResultAwaitedElement awaited = this->awaitedElement(type);
  bool reported = awaited.isErr() &&
                  awaited.error().code == AsyncCheckerAwaitedElementErrorCode::ALREADY_REPORTED;
  if (awaited.isOk()) {
    element = awaited.value();
  } else if (!reported) {
    this->state.getDiagnostics().error(stmt->iterable->loc(), awaited.error().message);
  }
  if (awaited.isOk() && type->isIterator()) {
    std::string label = ErrorTextAsyncContext::awaitLoop();
    GeneratorChecker::ResultNextNeedsNoValue nothing =
        GeneratorChecker(this->state).checkSendsNothing(type, label);
    if (nothing.isErr()) {
      this->state.getDiagnostics().error(stmt->iterable->loc(), nothing.error().message);
    }
  }
  ResultBindingUntyped typed = AsyncChecker::bindingType(stmt->binding);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(stmt->binding.typeAnn->loc, typed.error().message);
  }
  ResultBindingPattern shape = AsyncChecker::bindingShape(stmt->binding, element);
  if (shape.isErr()) {
    this->state.getDiagnostics().error(stmt->binding.loc, shape.error().message);
    element = Type::error();
  }
  FlowChecker(this->state).calledFunction();
  LoopScan scan = LoopScanner(this->state).scanLoop({stmt->body}, {});
  scan.callsFunctions = true;
  stmt->borrowElements = false;
  Flow head = LoopBounds(this->state).loopHead(scan, nullptr, false);
  this->state.getFlowState().replaceFlow(head);
  this->state.getScopeStack().openScope();
  stmt->binding.sym =
      Scopes(this->state).declareVar(stmt->binding.name, stmt->binding.loc, element, stmt->isConst);
  LoopChecker(this->state).checkLoopBody(stmt->body, true);
  Scopes(this->state).closeScope();
  this->state.getFlowState().replaceFlow(head);
  FlowChecker(this->state).calledFunction();
}

AsyncChecker::ResultPromiseHandled
AsyncChecker::checkDiscardedPromise(const Expr *expr, bool callback, llvm::SMLoc at) {
  const Type *type = expr->type;
  if (isa<AssignExpr>(expr)) {
    return ok(expr);
  }
  auto *call = dyn_cast<CallExpr>(expr);
  if (call && (call->builtin() == Builtin::ArrPop || call->builtin() == Builtin::ArrShift ||
               call->builtin() == Builtin::ArrSplice)) {
    return ok(expr);
  }
  bool mapped =
      call && (call->builtin() == Builtin::ArrMap || call->builtin() == Builtin::ArrFlatMap);
  if (!type->nonOptional()->isPromise() && mapped) {
    std::string message = ErrorTextDiscarded::mapped();

    return err(AsyncCheckerCheckDiscardedPromiseError{
        .code = AsyncCheckerCheckDiscardedPromiseErrorCode::PROMISES_DROPPED,
        .message = message,
    });
  }
  if (!type->nonOptional()->isPromise()) {
    return ok(expr);
  }
  if (callback) {
    std::string message = ErrorTextDiscarded::callback();

    return err(AsyncCheckerCheckDiscardedPromiseError{
        .code = AsyncCheckerCheckDiscardedPromiseErrorCode::CALLBACK_DROPS,
        .message = message,
    });
  }
  std::string text = AsyncChecker::statementText(at);
  if (expr->type && expr->type->isPromise() && expr->type->promised()->holdsResults()) {
    std::string shown = text.empty() ? std::string("p") : text;
    std::string message = ErrorTextDiscarded::result(shown);

    return err(AsyncCheckerCheckDiscardedPromiseError{
        .code = AsyncCheckerCheckDiscardedPromiseErrorCode::RESULT_UNCHECKED,
        .message = message,
    });
  }
  std::string message =
      text.empty() ? ErrorTextDiscarded::plain() : ErrorTextDiscarded::withText(text);

  return err(AsyncCheckerCheckDiscardedPromiseError{
      .code = AsyncCheckerCheckDiscardedPromiseErrorCode::NOT_AWAITED,
      .message = message,
  });
}

}
