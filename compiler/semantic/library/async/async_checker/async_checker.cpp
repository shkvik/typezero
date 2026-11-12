#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker_errors.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/iterators/iterable_checker/iterable_checker.h"
#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/bodies/stmt/loop_checker/loop_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/loop_bounds/loop_bounds.h"
#include "compiler/semantic/flow/loop_scanner/loop_scanner.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/generics/generic_widening/generic_widening.h"
#include "compiler/semantic/library/regex/match_checker/match_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr size_t kMaxShownStatement = 60;
constexpr llvm::StringLiteral kPromisePlaceholder = "p";

}

AsyncChecker::AsyncChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->loopBounds = &all.loopBounds;
  this->dictChecker = &all.dictChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->loopChecker = &all.loopChecker;
  this->loopScanner = &all.loopScanner;
  this->matchChecker = &all.matchChecker;
  this->typeResolver = &all.typeResolver;
  this->typeRelations = &all.typeRelations;
  this->genericChecker = &all.genericChecker;
  this->genericWidening = &all.genericWidening;
  this->iterableChecker = &all.iterableChecker;
  this->iteratorMethods = &all.iteratorMethods;
  this->generatorChecker = &all.generatorChecker;
}

const Type *AsyncChecker::withoutPromise(const Type *type) const {
  return type->isPromise() ? type->promised() : type;
}

std::string AsyncChecker::statementText(llvm::SMLoc at) const {
  if (!at.isValid()) {
    return "";
  }
  const char *start = at.getPointer();
  const char *end = start;
  int depth = 0;
  for (; *end != '\0' && *end != '\n' && *end != '\r'; ++end) {
    bool isComment = end[0] == '/' && end[1] == '/';
    if (depth == 0 && (*end == ';' || isComment)) {
      break;
    }
    depth += this->depthChange(*end);
  }
  llvm::StringRef text = llvm::StringRef(start, size_t(end - start)).rtrim();
  if (depth != 0 || text.size() > kMaxShownStatement) {
    return "";
  }

  return text.str();
}

int AsyncChecker::depthChange(char symbol) const {
  if (symbol == '(' || symbol == '[' || symbol == '{') {
    return 1;
  }
  if (symbol == ')' || symbol == ']' || symbol == '}') {
    return -1;
  }

  return 0;
}

AsyncChecker::ResultPromiseTypeRef AsyncChecker::resolvePromiseType(const TypeRef &ref) {
  const auto *builtin = TypeNameTable::find(ref.name);
  bool isPromiseName = builtin != nullptr && builtin->form.is<PromiseTypeName>();
  if (!isPromiseName) {
    return err(AsyncCheckerResolvePromiseTypeError{
        .code = AsyncCheckerResolvePromiseTypeErrorCode::NOT_A_PROMISE,
        .message = "",
    });
  }
  if (ref.args.size() != 1) {
    auto message = ErrorTextPromiseType::typeArgs(ref.name);

    return err(AsyncCheckerResolvePromiseTypeError{
        .code = AsyncCheckerResolvePromiseTypeErrorCode::WRONG_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  const auto *written = this->typeResolver->resolveTypeOrVoid(ref.args[0]);
  const auto *value = this->withoutPromise(written);
  const auto *promise = Type::promise(value);

  return ok(promise);
}

AsyncChecker::ResultReturnsPromise
AsyncChecker::checkPromiseDeclared(const FunctionDecl *functionDecl, const Type *declared) const {
  bool isPlainAsync = functionDecl->isAsync && !functionDecl->isGenerator;
  if (isPlainAsync && !declared->isError() && !declared->isPromise()) {
    auto message = ErrorTextPromiseType::notPromise(declared->name());

    return err(AsyncCheckerPromiseDeclaredError{
        .code = AsyncCheckerPromiseDeclaredErrorCode::NOT_A_PROMISE,
        .message = message,
    });
  }

  return ok(declared);
}

const Type *AsyncChecker::asyncReturnType(const FunctionDecl *functionDecl, const Type *declared) {
  auto promise = this->checkPromiseDeclared(functionDecl, declared);
  if (promise.isOk()) {
    return declared;
  }
  this->state.getDiagnostics().error(functionDecl->returnType->loc, promise.error().message);

  return Type::promise(declared);
}

AsyncChecker::ResultReturnsOneType
AsyncChecker::checkReturnsJoin(const Type *type, const Type *candidate,
                               const FuncSymbol *function) const {
  const Type *common = type != nullptr ? this->typeRelations->unify(type, candidate) : candidate;
  if (common == nullptr || common->isError()) {
    auto name = ErrorTextFunctionName::quoted(function->name);
    auto message = ErrorTextAsyncReturns::differ(name, candidate->name(), type->name());

    return err(AsyncCheckerReturnsJoinError{
        .code = AsyncCheckerReturnsJoinErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(common);
}

const Type *AsyncChecker::inferAsyncType(const FuncSymbol *function,
                                         llvm::ArrayRef<ReturnStmt *> returns, bool isFallingOff,
                                         ReturnErrors errors) {
  auto sorted = this->sortedReturns(returns);
  const Type *type = nullptr;
  for (const ReturnStmt *returnStmt : sorted.valued) {
    const auto *candidate = this->withoutPromise(returnStmt->value->type);
    auto joined = this->checkReturnsJoin(type, candidate, function);
    if (joined.isErr() && errors == ReturnErrors::REPORTED) {
      this->state.getDiagnostics().error(returnStmt->value->loc(), joined.error().message);
    }
    if (joined.isErr()) {
      return Type::error();
    }
    type = joined.value();
  }
  if (type != nullptr) {
    bool isOptional = isFallingOff || sorted.hasNothing;
    const Type *value = isOptional ? Type::optional(type) : type;

    return Type::promise(value);
  }
  if (!sorted.hasUnknown) {
    return Type::promise(Type::voidTy());
  }

  return errors == ReturnErrors::REPORTED ? Type::error() : nullptr;
}

AsyncChecker::SortedReturns
AsyncChecker::sortedReturns(llvm::ArrayRef<ReturnStmt *> returns) const {
  auto sorted = SortedReturns{.valued = {}, .hasNothing = false, .hasUnknown = false};
  for (const ReturnStmt *returnStmt : returns) {
    switch (this->returnedKind(returnStmt)) {
      case Returned::NOTHING:
        sorted.hasNothing = true;
        break;
      case Returned::UNKNOWN:
        sorted.hasUnknown = true;
        break;
      case Returned::VALUE:
        sorted.valued.push_back(returnStmt);
        break;
    }
  }

  return sorted;
}

AsyncChecker::Returned AsyncChecker::returnedKind(const ReturnStmt *returnStmt) const {
  if (returnStmt->value == nullptr) {
    return Returned::NOTHING;
  }
  const auto *settled = this->withoutPromise(returnStmt->value->type);
  if (settled->isError()) {
    return Returned::UNKNOWN;
  }
  if (settled->isUndefined() || settled->isVoid()) {
    return Returned::NOTHING;
  }

  return Returned::VALUE;
}

AsyncChecker::ResultPromiseMatches
AsyncChecker::checkPromiseMatches(const Type *given, const Type *value,
                                  const FuncSymbol *function) const {
  if (given != value && !given->isNever()) {
    auto name = ErrorTextFunctionName::quoted(function->name);
    auto message = ErrorTextAsyncReturns::promise(given->name(), name, value->name());

    return err(AsyncCheckerPromiseMatchesError{
        .code = AsyncCheckerPromiseMatchesErrorCode::OTHER_PROMISE_TYPE,
        .message = message,
    });
  }

  return ok(given);
}

void AsyncChecker::finishAsyncReturns(FuncSymbol *function, llvm::ArrayRef<ReturnStmt *> returns) {
  const Type *returned = function->getKnownReturnType();
  if (!returned->isPromise()) {
    return;
  }
  const Type *value = returned->promised();
  if (value == nullptr || value->isError()) {
    return;
  }
  for (ReturnStmt *returnStmt : returns) {
    bool isTyped = returnStmt->value != nullptr && !returnStmt->value->type->isError();
    if (isTyped) {
      this->finishAsyncReturn(function, returnStmt, value);
    }
  }
}

void AsyncChecker::finishAsyncReturn(const FuncSymbol *function, ReturnStmt *returnStmt,
                                     const Type *value) {
  const Type *type = returnStmt->value->type;
  if (type->isPromise()) {
    auto matches = this->checkPromiseMatches(type->promised(), value, function);
    if (matches.isErr()) {
      this->state.getDiagnostics().error(returnStmt->value->loc(), matches.error().message);
    }
    return;
  }
  if (!value->isVoid()) {
    returnStmt->value = this->typeRelations->convert(returnStmt->value, value);
  }
}

AsyncChecker::ResultInAsyncContext AsyncChecker::checkAwaitContext() const {
  auto *function = this->state.getFunctionStack().getCurrentFunction();
  if (this->isPlainFunction(function)) {
    auto hint = this->asyncHint(function->decl);
    auto message = ErrorTextAsyncContext::await(hint);

    return err(AsyncCheckerAsyncContextError{
        .code = AsyncCheckerAsyncContextErrorCode::AWAIT_NOT_ASYNC,
        .message = message,
    });
  }

  return ok(function);
}

AsyncChecker::ResultInAsyncContext AsyncChecker::checkForAwaitContext() const {
  auto *function = this->state.getFunctionStack().getCurrentFunction();
  if (this->isPlainFunction(function)) {
    auto message = ErrorTextAsyncContext::forAwait();

    return err(AsyncCheckerAsyncContextError{
        .code = AsyncCheckerAsyncContextErrorCode::FOR_AWAIT_NOT_ASYNC,
        .message = message,
    });
  }

  return ok(function);
}

bool AsyncChecker::isPlainFunction(const FuncSymbol *function) const {
  if (function == nullptr) {
    return false;
  }
  bool isAsync = function->decl != nullptr && function->decl->isAsync;

  return !isAsync;
}

std::string AsyncChecker::asyncHint(const FunctionDecl *functionDecl) const {
  if (functionDecl == nullptr) {
    return ErrorTextAsyncContext::plainHint();
  }
  if (functionDecl->isArrow()) {
    return ErrorTextAsyncContext::arrowHint();
  }
  if (functionDecl->cls() != nullptr) {
    return ErrorTextAsyncContext::methodHint(functionDecl->name);
  }
  if (!functionDecl->name.empty()) {
    return ErrorTextAsyncContext::namedHint(functionDecl->name);
  }

  return ErrorTextAsyncContext::plainHint();
}

void AsyncChecker::walkContext(ResultInAsyncContext context, llvm::SMLoc loc) {
  if (context.isErr()) {
    this->state.getDiagnostics().error(loc, context.error().message);
    return;
  }
  if (context.value() == nullptr) {
    this->state.getInventoryStore().markTopLevelAwait();
  }
}

AsyncChecker::ResultAwaitedOneType AsyncChecker::checkAwaitable(const Type *type) const {
  const auto *result = AsyncChecker::awaitedType(type);
  if (result == nullptr) {
    auto message = ErrorTextAwaited::mixed(type->name());

    return err(AsyncCheckerAwaitableError{
        .code = AsyncCheckerAwaitableErrorCode::MIXED_PROMISE,
        .message = message,
    });
  }

  return ok(result);
}

Expr *AsyncChecker::checkAwait(AwaitExpr *await, const Type *expected) {
  await->type = Type::error();
  this->state.getInventoryStore().markUsesAsync();
  auto context = this->checkAwaitContext();
  this->walkContext(context, await->loc());
  const Type *hint = this->promiseHint(await->value, expected);
  await->value = this->exprChecker->checkExpr(await->value, hint);
  const Type *type = await->value->type;
  this->flowChecker->calledFunction();
  if (type->isError()) {
    return await;
  }
  auto result = this->checkAwaitable(type);
  if (result.isErr()) {
    this->state.getDiagnostics().error(await->value->loc(), result.error().message);

    return await;
  }
  await->type = result.value();

  return await;
}

const Type *AsyncChecker::promiseHint(const Expr *value, const Type *expected) const {
  bool isExpected = expected != nullptr && !expected->isError() && !expected->isVoid();
  bool isMaking = isa<NewExpr>(value) || isa<CallExpr>(value);
  if (isExpected && isMaking) {
    return Type::promise(expected);
  }

  return nullptr;
}

const Type *AsyncChecker::awaitedType(const Type *type) {
  int optionals = 0;
  const Type *inner = type;
  while (inner->isOptional()) {
    optionals += 1;
    inner = inner->inner();
  }
  const auto *awaited = AsyncChecker::awaitedPlain(inner);
  for (int index = 0; index < optionals; ++index) {
    if (awaited == nullptr || awaited->isVoid()) {
      return awaited;
    }
    awaited = Type::optional(awaited);
  }

  return awaited;
}

const Type *AsyncChecker::awaitedPlain(const Type *type) {
  if (type->isPromise()) {
    return type->promised();
  }
  if (!type->isUnion() || !AsyncChecker::hasPromiseMember(type)) {
    return type;
  }
  const Type *result = nullptr;
  for (const Type *member : type->members()) {
    const Type *candidate = member->isPromise() ? member->promised() : member;
    result = result != nullptr ? LiteralTypes::unionUnify(result, candidate) : candidate;
    if (result == nullptr || result->isError()) {
      return nullptr;
    }
  }

  return result;
}

bool AsyncChecker::hasPromiseMember(const Type *type) {
  for (const Type *member : type->members()) {
    if (member->isPromise()) {
      return true;
    }
  }

  return false;
}

AsyncChecker::ResultAwaitedElement AsyncChecker::checkAwaitedElement(const Type *type) const {
  const auto *element = this->generatorChecker->asyncIteratedType(type);
  if (element != nullptr) {
    return ok(element);
  }
  if (this->isOptionalAsyncIterable(type)) {
    auto message = ErrorTextAwaitedElement::undefined(type->name());

    return err(AsyncCheckerAwaitedElementError{
        .code = AsyncCheckerAwaitedElementErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type->isRecord()) {
    auto message = ErrorTextAwaitedElement::record();

    return err(AsyncCheckerAwaitedElementError{
        .code = AsyncCheckerAwaitedElementErrorCode::RECORD_NOT_ITERABLE,
        .message = message,
    });
  }
  if (!type->isError()) {
    auto message = ErrorTextAwaitedElement::notAsync(type->name());

    return err(AsyncCheckerAwaitedElementError{
        .code = AsyncCheckerAwaitedElementErrorCode::NOT_ASYNC_ITERABLE,
        .message = message,
    });
  }

  return err(AsyncCheckerAwaitedElementError{
      .code = AsyncCheckerAwaitedElementErrorCode::ALREADY_REPORTED,
      .message = "",
  });
}

bool AsyncChecker::isOptionalAsyncIterable(const Type *type) const {
  if (!type->isOptional()) {
    return false;
  }
  const auto *element = this->generatorChecker->asyncIteratedType(type->inner());

  return element != nullptr;
}

AsyncChecker::ResultBindingUntyped
AsyncChecker::checkBindingType(const VarDeclarator &binding) const {
  if (binding.typeAnn.has_value()) {
    auto message = ErrorTextBindingType::annotated();

    return err(AsyncCheckerBindingTypeError{
        .code = AsyncCheckerBindingTypeErrorCode::HAS_TYPE_ANNOTATION,
        .message = message,
    });
  }

  return ok(&binding);
}

AsyncChecker::ResultBindingPattern AsyncChecker::checkBindingShape(const VarDeclarator &binding,
                                                                   const Type *element) const {
  bool isListLike =
      element->isArray() || element->isTuple() || element->isMatch() || element->isError();
  bool isFitting = !binding.destructured() || binding.objectPattern() || isListLike;
  if (!isFitting) {
    auto message = ErrorTextBindingShape::notArray(element->name());

    return err(AsyncCheckerBindingShapeError{
        .code = AsyncCheckerBindingShapeErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(element);
}

void AsyncChecker::walkForAwait(ForOfStmt *stmt) {
  this->state.getInventoryStore().markUsesAsync();
  auto context = this->checkForAwaitContext();
  this->walkContext(context, stmt->loc());
  stmt->iterable = this->checkedIterable(stmt->iterable);
  const Type *element = this->loopElement(stmt->iterable);
  const Type *bound = this->boundElement(stmt->binding, element);
  this->walkAwaitBody(stmt, bound);
}

Expr *AsyncChecker::checkedIterable(Expr *iterable) {
  this->iteratorMethods->markFused(iterable);
  auto *checked = this->exprChecker->checkExpr(iterable);
  auto *apparent = this->genericChecker->apparent(checked);
  auto *joined = this->genericWidening->joined(apparent);
  auto *source = this->asyncSource(joined);
  if (source->type->isMatch()) {
    return this->matchChecker->matchItems(source);
  }

  return source;
}

Expr *AsyncChecker::asyncSource(Expr *iterable) {
  auto *iterator = this->iterableChecker->asyncUserIterator(iterable);
  if (iterator != nullptr) {
    return iterator;
  }

  return this->dictChecker->iterableOf(iterable);
}

const Type *AsyncChecker::loopElement(const Expr *iterable) {
  auto awaited = this->checkAwaitedElement(iterable->type);
  if (awaited.isOk()) {
    this->walkSendsNothing(iterable);

    return awaited.value();
  }
  if (awaited.error().code != AsyncCheckerAwaitedElementErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(iterable->loc(), awaited.error().message);
  }

  return Type::error();
}

void AsyncChecker::walkSendsNothing(const Expr *iterable) {
  const Type *type = iterable->type;
  if (!type->isIterator()) {
    return;
  }
  auto label = ErrorTextAsyncContext::awaitLoop();
  auto nothing = this->generatorChecker->checkSendsNothing(type, label);
  if (nothing.isErr()) {
    this->state.getDiagnostics().error(iterable->loc(), nothing.error().message);
  }
}

const Type *AsyncChecker::boundElement(const VarDeclarator &binding, const Type *element) {
  auto typed = this->checkBindingType(binding);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(binding.typeAnn->loc, typed.error().message);
  }
  auto shape = this->checkBindingShape(binding, element);
  if (shape.isErr()) {
    this->state.getDiagnostics().error(binding.loc, shape.error().message);

    return Type::error();
  }

  return element;
}

void AsyncChecker::walkAwaitBody(ForOfStmt *stmt, const Type *element) {
  this->flowChecker->calledFunction();
  auto scan = this->loopScanner->scanForOfLoop(stmt);
  scan.isCallingFunctions = true;
  stmt->borrowElements = false;
  auto head = this->loopBounds->loopHead(scan);
  this->state.getFlowState().replaceFlow(head);
  this->state.getScopeStack().openScope();
  VarDeclarator &binding = stmt->binding;
  binding.sym = this->scopes->declareVar(binding.name, binding.loc, element, stmt->isConst);
  this->loopChecker->walkAwaitLoopBody(stmt->body);
  this->scopes->closeScope();
  this->state.getFlowState().replaceFlow(head);
  this->flowChecker->calledFunction();
}

AsyncChecker::ResultPromiseHandled
AsyncChecker::checkDiscardedPromise(const Expr *expr, bool isCallback, llvm::SMLoc at) {
  if (isa<AssignExpr>(expr) || this->isTakingElement(expr)) {
    return ok(expr);
  }
  const Type *type = expr->type;
  bool isPromise = type->nonOptional()->isPromise();
  if (!isPromise && this->isMappingCall(expr)) {
    auto message = ErrorTextDiscarded::mapped();

    return err(AsyncCheckerCheckDiscardedPromiseError{
        .code = AsyncCheckerCheckDiscardedPromiseErrorCode::PROMISES_DROPPED,
        .message = message,
    });
  }
  if (!isPromise) {
    return ok(expr);
  }
  if (isCallback) {
    auto message = ErrorTextDiscarded::callback();

    return err(AsyncCheckerCheckDiscardedPromiseError{
        .code = AsyncCheckerCheckDiscardedPromiseErrorCode::CALLBACK_DROPS,
        .message = message,
    });
  }
  auto text = this->statementText(at);
  if (type->isPromise() && type->promised()->holdsResults()) {
    std::string shown = text.empty() ? kPromisePlaceholder.str() : text;
    auto message = ErrorTextDiscarded::result(shown);

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

bool AsyncChecker::isTakingElement(const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr) {
    return false;
  }
  Builtin builtin = call->builtin();

  return builtin == Builtin::ArrPop || builtin == Builtin::ArrShift ||
         builtin == Builtin::ArrSplice;
}

bool AsyncChecker::isMappingCall(const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr) {
    return false;
  }
  Builtin builtin = call->builtin();

  return builtin == Builtin::ArrMap || builtin == Builtin::ArrFlatMap;
}

}
