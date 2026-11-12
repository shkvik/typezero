#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/iterator/iterator_method_table/iterator_method_table.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker_errors.h"
#include "compiler/semantic/bodies/iterators/iterable_checker/iterable_checker.h"
#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

GeneratorChecker::GeneratorChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->dictChecker = &all.dictChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->typeDeclarer = &all.typeDeclarer;
  this->typeResolver = &all.typeResolver;
  this->typeRelations = &all.typeRelations;
  this->iteratorMethods = &all.iteratorMethods;
  this->iterableChecker = &all.iterableChecker;
}

GeneratorChecker::ResultResolvedIterator GeneratorChecker::resolveIteratorType(const TypeRef &ref) {
  llvm::StringRef name = ref.name;
  if (TypeNameTable::isName(name, TypeName::ITERATOR_RESULT) &&
      this->typeDeclarer->lookupType(name) == nullptr) {
    return this->resolveIteratorResult(ref);
  }
  if (TypeNameTable::isName(name, TypeName::ITERABLE) &&
      this->typeDeclarer->lookupType(name) == nullptr) {
    auto message = ErrorTextIteratorType::iterable();

    return err(GeneratorCheckerResolveIteratorTypeError{
        .code = GeneratorCheckerResolveIteratorTypeErrorCode::ITERABLE_TYPE,
        .message = message,
    });
  }
  const auto *builtin = TypeNameTable::find(name);
  const IteratorTypeName *iteratorName =
      builtin != nullptr ? builtin->form.as<IteratorTypeName>() : nullptr;
  bool isShadowed = TypeNameTable::isName(name, TypeName::ITERATOR) &&
                    this->typeDeclarer->lookupType(name) != nullptr;
  if (iteratorName == nullptr || isShadowed) {
    return err(GeneratorCheckerResolveIteratorTypeError{
        .code = GeneratorCheckerResolveIteratorTypeErrorCode::NOT_AN_ITERATOR,
        .message = "",
    });
  }
  if (ref.args.empty() || ref.args.size() > 3) {
    auto message = ErrorTextIteratorType::typeArgs(name);

    return err(GeneratorCheckerResolveIteratorTypeError{
        .code = GeneratorCheckerResolveIteratorTypeErrorCode::WRONG_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return this->resolveIteratorArgs(ref, iteratorName->async);
}

GeneratorChecker::ResultResolvedIterator
GeneratorChecker::resolveIteratorResult(const TypeRef &ref) {
  if (ref.args.empty() || ref.args.size() > 2) {
    auto message = ErrorTextIteratorType::resultArgs();

    return err(GeneratorCheckerResolveIteratorTypeError{
        .code = GeneratorCheckerResolveIteratorTypeErrorCode::RESULT_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  const auto *value = this->typeResolver->resolveType(ref.args[0]);
  const auto *returned = this->typeArgOr(ref, 1, Type::undefinedTy());
  if (value->isError() || returned->isError()) {
    return ok(Type::error());
  }
  const auto *result = this->iterableChecker->iteratorResultType(value, returned);

  return ok(result);
}

GeneratorChecker::ResultResolvedIterator GeneratorChecker::resolveIteratorArgs(const TypeRef &ref,
                                                                               bool isAsync) {
  const auto *yielded = this->typeResolver->resolveType(ref.args[0]);
  const auto *returned = this->typeArgOr(ref, 1, Type::voidTy());
  const auto *sent = this->typeArgOr(ref, 2, Type::voidTy());
  for (const Type *type : {yielded, returned, sent}) {
    if (type->isError()) {
      return ok(type);
    }
  }
  const Type *iterator = isAsync ? Type::asyncIterator(yielded, returned, sent)
                                 : Type::iterator(yielded, returned, sent);

  return ok(iterator);
}

const Type *GeneratorChecker::typeArgOr(const TypeRef &ref, size_t index, const Type *fallback) {
  if (index >= ref.args.size()) {
    return fallback;
  }
  const TypeRef &arg = ref.args[index];
  if (this->isNamingNoValue(arg)) {
    return fallback;
  }

  return this->typeResolver->resolveType(arg);
}

bool GeneratorChecker::isNamingNoValue(const TypeRef &arg) const {
  if (arg.kind() != TypeRefKind::NAME_REF) {
    return false;
  }
  bool isVoid = TypeNameTable::isName(arg.name, TypeName::VOID_TYPE);
  bool isUndefined = TypeNameTable::isName(arg.name, TypeName::UNDEFINED);
  bool isUnknown = TypeNameTable::isName(arg.name, TypeName::UNKNOWN);

  return isVoid || isUndefined || isUnknown;
}

GeneratorChecker::ResultGeneratorReturns
GeneratorChecker::checkGeneratorReturnType(const FunctionDecl *functionDecl, const Type *declared) {
  bool isPlain = !functionDecl->isGenerator || declared->isError();
  if (!isPlain && functionDecl->isAsync && !declared->isAsyncIterator()) {
    const Type *yielded = declared->isIterator() ? declared->yielded() : declared;
    auto shown = this->shownElementName(yielded);
    auto message = ErrorTextGeneratorType::async(shown);

    return err(GeneratorCheckerGeneratorReturnTypeError{
        .code = GeneratorCheckerGeneratorReturnTypeErrorCode::ASYNC_NEEDS_ASYNC_ITERATOR,
        .message = message,
    });
  }
  if (!isPlain && !functionDecl->isAsync && !declared->isIterator()) {
    auto shown = this->shownElementName(declared);
    auto message = ErrorTextGeneratorType::sync(shown);

    return err(GeneratorCheckerGeneratorReturnTypeError{
        .code = GeneratorCheckerGeneratorReturnTypeErrorCode::NEEDS_ITERATOR,
        .message = message,
    });
  }

  return ok(declared);
}

std::string GeneratorChecker::shownElementName(const Type *element) const {
  if (element->isVoid()) {
    return Type::intTy()->name().str();
  }

  return element->name().str();
}

std::string GeneratorChecker::shownYieldedName(const Type *yielded) const {
  if (yielded == nullptr) {
    return Type::intTy()->name().str();
  }

  return yielded->name().str();
}

GeneratorChecker::ResultYieldedTypesJoin
GeneratorChecker::checkYieldsJoin(const Type *yielded, const Type *type,
                                  const FuncSymbol *function) const {
  const Type *common = yielded != nullptr ? LiteralTypes::unionUnify(yielded, type) : type;
  if (common == nullptr || common->isError()) {
    auto name = ErrorTextFunctionName::quoted(function->name);
    auto message = ErrorTextYields::differ(name, type->name(), yielded->name());

    return err(GeneratorCheckerYieldsJoinError{
        .code = GeneratorCheckerYieldsJoinErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(common);
}

GeneratorChecker::ResultReturnsOfOneType
GeneratorChecker::checkReturnsJoin(const Type *returned, const Type *type,
                                   const FuncSymbol *function) const {
  const Type *common = returned != nullptr ? LiteralTypes::unionUnify(returned, type) : type;
  if (common == nullptr || common->isError()) {
    auto name = ErrorTextFunctionName::quoted(function->name);
    auto message = ErrorTextAsyncReturns::differ(name, type->name(), returned->name());

    return err(GeneratorCheckerReturnsJoinError{
        .code = GeneratorCheckerReturnsJoinErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(common);
}

GeneratorChecker::ResultYieldedTypeKnown
GeneratorChecker::checkYieldsKnown(const Type *yielded, const FuncSymbol *function,
                                   bool hasNoYields) const {
  if (yielded == nullptr || yielded->isUndefined()) {
    auto why = this->nothingYieldedReason(yielded, hasNoYields);
    auto name = ErrorTextFunctionName::quoted(function->name);
    auto message = ErrorTextYields::unknown(name, why);

    return err(GeneratorCheckerYieldsKnownError{
        .code = GeneratorCheckerYieldsKnownErrorCode::NOTHING_YIELDED,
        .message = message,
    });
  }

  return ok(yielded);
}

std::string GeneratorChecker::nothingYieldedReason(const Type *yielded, bool hasNoYields) const {
  if (hasNoYields) {
    return ErrorTextYields::noYield();
  }
  if (yielded != nullptr) {
    return ErrorTextYields::onlyUndef();
  }

  return "";
}

const Type *GeneratorChecker::inferGeneratorType(const FuncSymbol *function,
                                                 llvm::ArrayRef<YieldExpr *> yields,
                                                 llvm::ArrayRef<ReturnStmt *> returns,
                                                 bool isFallingOff, ReturnErrors errors) {
  bool isAsync = function->decl != nullptr && function->decl->isAsync;
  auto join = this->joinYields(function, yields, isAsync, errors);
  if (join.hasFailed) {
    return Type::error();
  }
  if (join.yielded == nullptr || join.yielded->isUndefined()) {
    return this->unknownYields(function, yields, join, errors);
  }
  auto returned = this->joinedReturns(function, returns, isFallingOff, isAsync, errors);
  if (!returned.has_value()) {
    return Type::error();
  }
  if (isAsync) {
    return Type::asyncIterator(join.yielded, *returned, Type::voidTy());
  }

  return Type::iterator(join.yielded, *returned, Type::voidTy());
}

GeneratorChecker::YieldJoin GeneratorChecker::joinYields(const FuncSymbol *function,
                                                         llvm::ArrayRef<YieldExpr *> yields,
                                                         bool isAsync, ReturnErrors errors) {
  auto known = this->knownYields(yields, isAsync);
  bool isBroken = known.size() != yields.size();
  const Type *yielded = nullptr;
  for (const KnownYield &value : known) {
    auto joined = this->checkYieldsJoin(yielded, value.type, function);
    if (joined.isErr() && errors == ReturnErrors::REPORTED) {
      this->state.getDiagnostics().error(value.yield->loc(), joined.error().message);
    }
    if (joined.isErr()) {
      return YieldJoin{
          .yielded = yielded,
          .isBroken = isBroken,
          .hasFailed = true,
      };
    }
    yielded = joined.value();
  }

  return YieldJoin{
      .yielded = yielded,
      .isBroken = isBroken,
      .hasFailed = false,
  };
}

std::vector<GeneratorChecker::KnownYield>
GeneratorChecker::knownYields(llvm::ArrayRef<YieldExpr *> yields, bool isAsync) const {
  auto known = std::vector<KnownYield>();
  for (const YieldExpr *yield : yields) {
    const auto *type = this->yieldedValueType(yield, isAsync);
    if (type != nullptr && !type->isError()) {
      known.push_back(KnownYield{.yield = yield, .type = type});
    }
  }

  return known;
}

const Type *GeneratorChecker::yieldedValueType(const YieldExpr *yield, bool isAsync) const {
  if (yield->delegate) {
    return yield->element;
  }
  const Type *type = yield->value != nullptr ? yield->value->type : Type::undefinedTy();
  if (!isAsync || type == nullptr || type->isError()) {
    return type;
  }

  return AsyncChecker::awaitedType(type);
}

const Type *GeneratorChecker::unknownYields(const FuncSymbol *function,
                                            llvm::ArrayRef<YieldExpr *> yields,
                                            const YieldJoin &join, ReturnErrors errors) {
  if (errors == ReturnErrors::IGNORED) {
    return nullptr;
  }
  if (join.isBroken && join.yielded == nullptr) {
    return Type::error();
  }
  auto known = this->checkYieldsKnown(join.yielded, function, yields.empty());
  if (known.isErr()) {
    this->state.getDiagnostics().error(function->loc, known.error().message);
  }

  return Type::error();
}

std::optional<const Type *> GeneratorChecker::joinedReturns(const FuncSymbol *function,
                                                            llvm::ArrayRef<ReturnStmt *> returns,
                                                            bool isFallingOff, bool isAsync,
                                                            ReturnErrors errors) {
  bool isUndefined = isFallingOff || this->hasUndefinedReturn(returns, isAsync);
  const Type *returned = nullptr;
  auto known = this->knownReturns(returns, isAsync);
  for (const KnownReturn &value : known) {
    auto joined = this->checkReturnsJoin(returned, value.type, function);
    if (joined.isErr() && errors == ReturnErrors::REPORTED) {
      this->state.getDiagnostics().error(value.returnStmt->value->loc(), joined.error().message);
    }
    if (joined.isErr()) {
      return std::nullopt;
    }
    returned = joined.value();
  }
  if (returned == nullptr) {
    return Type::voidTy();
  }
  if (isUndefined && !returned->isVoid()) {
    return Type::optional(returned);
  }

  return returned;
}

bool GeneratorChecker::hasUndefinedReturn(llvm::ArrayRef<ReturnStmt *> returns,
                                          bool isAsync) const {
  for (const ReturnStmt *returnStmt : returns) {
    if (this->returnedValueType(returnStmt, isAsync) == nullptr) {
      return true;
    }
  }

  return false;
}

std::vector<GeneratorChecker::KnownReturn>
GeneratorChecker::knownReturns(llvm::ArrayRef<ReturnStmt *> returns, bool isAsync) const {
  auto known = std::vector<KnownReturn>();
  for (const ReturnStmt *returnStmt : returns) {
    const auto *type = this->returnedValueType(returnStmt, isAsync);
    if (type != nullptr && !type->isError()) {
      known.push_back(KnownReturn{.returnStmt = returnStmt, .type = type});
    }
  }

  return known;
}

const Type *GeneratorChecker::returnedValueType(const ReturnStmt *returnStmt, bool isAsync) const {
  if (returnStmt->value == nullptr || returnStmt->value->type->isUndefined()) {
    return nullptr;
  }
  const Type *type = returnStmt->value->type;
  if (type->isError() || !isAsync) {
    return type;
  }
  const auto *awaited = AsyncChecker::awaitedType(type);
  if (awaited == nullptr || awaited->isUndefined()) {
    return nullptr;
  }

  return awaited;
}

const Type *GeneratorChecker::iteratedType(const Type *type) const {
  if (type->isArray()) {
    return type->inner();
  }
  if (type->isString()) {
    return type;
  }
  if (type->isIterator()) {
    return type->yielded();
  }
  if (type->isTypedArray()) {
    return type->elementType();
  }
  if (type->isParam() && type->iterated() != nullptr) {
    return type->iterated();
  }

  return nullptr;
}

const Type *GeneratorChecker::asyncIteratedType(const Type *type) {
  if (type->isAsyncIterator()) {
    return type->yielded();
  }
  if (type->runtimeKind() == Type::Runtime::Readline ||
      type->runtimeKind() == Type::Runtime::ReadlinePromises) {
    return Type::string();
  }
  const auto *element = this->iteratedType(type);
  if (element == nullptr) {
    return nullptr;
  }
  const auto *awaited = AsyncChecker::awaitedType(element);

  return awaited != nullptr && awaited->isVoid() ? Type::undefinedTy() : awaited;
}

GeneratorChecker::ResultNextNeedsNoValue GeneratorChecker::checkSendsNothing(const Type *iterator,
                                                                             llvm::StringRef what) {
  const Type *sent = iterator->sent();
  bool isFree = sent->isVoid() || sent->isOptional() || sent->isError();
  if (!isFree) {
    auto message = ErrorTextSendsNothing::expects(what, iterator->name(), sent->name());

    return err(GeneratorCheckerCheckSendsNothingError{
        .code = GeneratorCheckerCheckSendsNothingErrorCode::EXPECTS_VALUE,
        .message = message,
    });
  }

  return ok(iterator);
}

GeneratorChecker::ResultNothingSentAsync
GeneratorChecker::checkAsyncSent(const Type *sent, const Type *yielded, bool isLoose) const {
  if (!sent->isVoid() && !isLoose) {
    auto shown = this->shownYieldedName(yielded);
    auto message = ErrorTextYield::asyncSent(shown);

    return err(GeneratorCheckerAsyncSentError{
        .code = GeneratorCheckerAsyncSentErrorCode::SENT_TO_ASYNC,
        .message = message,
    });
  }

  return ok(sent);
}

GeneratorChecker::ResultPromiseYieldFits
GeneratorChecker::checkPromiseYield(const Type *type, const Type *given,
                                    const YieldShape &shape) const {
  if (given == nullptr) {
    auto message = ErrorTextYield::mixed(type->name());

    return err(GeneratorCheckerPromiseYieldError{
        .code = GeneratorCheckerPromiseYieldErrorCode::MIXED_PROMISE,
        .message = message,
    });
  }
  bool isOther = shape.yielded != nullptr && !given->isError() && given != type &&
                 !TypeRelations::isImplicitlyConvertible(given, shape.yielded);
  if (isOther) {
    auto name = ErrorTextFunctionName::quoted(shape.function->name);
    auto message = ErrorTextYield::promise(given->name(), name, shape.yielded->name());

    return err(GeneratorCheckerPromiseYieldError{
        .code = GeneratorCheckerPromiseYieldErrorCode::OTHER_TYPE,
        .message = message,
    });
  }

  return ok(given);
}

GeneratorChecker::ResultYieldGivesAValue
GeneratorChecker::checkValueYielded(const YieldShape &shape) const {
  const Type *yielded = shape.yielded;
  if (yielded != nullptr && !yielded->isOptional() && !yielded->isError()) {
    auto name = ErrorTextFunctionName::quoted(shape.function->name);
    auto message = ErrorTextYield::noValue(name, yielded->name());

    return err(GeneratorCheckerValueYieldedError{
        .code = GeneratorCheckerValueYieldedErrorCode::UNDEFINED_YIELD,
        .message = message,
    });
  }

  return ok(yielded);
}

GeneratorChecker::ResultDelegateAsyncFit
GeneratorChecker::checkDelegateAsync(const Type *type, const Type *element,
                                     const YieldShape &shape) const {
  if (element == nullptr && !type->isError()) {
    auto message = ErrorTextYield::notAsync(type->name());

    return err(GeneratorCheckerDelegateAsyncError{
        .code = GeneratorCheckerDelegateAsyncErrorCode::NOT_ASYNC_ITERABLE,
        .message = message,
    });
  }
  bool isOther = element != nullptr && shape.yielded != nullptr &&
                 !TypeRelations::isImplicitlyConvertible(element, shape.yielded);
  if (isOther) {
    auto name = ErrorTextFunctionName::quoted(shape.function->name);
    auto message = ErrorTextYield::otherType(element->name(), name, shape.yielded->name());

    return err(GeneratorCheckerDelegateAsyncError{
        .code = GeneratorCheckerDelegateAsyncErrorCode::OTHER_TYPE,
        .message = message,
    });
  }

  return ok(element);
}

GeneratorChecker::ResultDelegateSyncFits
GeneratorChecker::checkDelegateSync(const Type *type, const Type *element,
                                    const YieldShape &shape) const {
  if (element == nullptr && !type->isError()) {
    auto message = ErrorTextYield::notIter(type->name());

    return err(GeneratorCheckerDelegateSyncError{
        .code = GeneratorCheckerDelegateSyncErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }
  bool isSending = element != nullptr && type->isIterator() && !type->sent()->isVoid() &&
                   type->sent() != shape.sent && !shape.isLoose;
  if (isSending) {
    auto name = ErrorTextFunctionName::quoted(shape.function->name);
    std::string gets = shape.sent->isVoid() ? ErrorTextYield::nothing()
                                            : ErrorTextFunctionName::quoted(shape.sent->name());
    auto message = ErrorTextYield::sends(type->name(), type->sent()->name(), name, gets);

    return err(GeneratorCheckerDelegateSyncError{
        .code = GeneratorCheckerDelegateSyncErrorCode::SENDS_DIFFER,
        .message = message,
    });
  }
  bool isOther = element != nullptr && shape.yielded != nullptr &&
                 !TypeRelations::isImplicitlyConvertible(element, shape.yielded);
  if (isOther) {
    auto name = ErrorTextFunctionName::quoted(shape.function->name);
    auto message = ErrorTextYield::otherType(element->name(), name, shape.yielded->name());

    return err(GeneratorCheckerDelegateSyncError{
        .code = GeneratorCheckerDelegateSyncErrorCode::OTHER_TYPE,
        .message = message,
    });
  }

  return ok(element);
}

GeneratorChecker::ResultSentTypeDeclared
GeneratorChecker::checkSentDeclared(const YieldExpr *yield, const YieldShape &shape) const {
  bool isUntyped = !yield->delegate && !shape.isAsync && shape.sent->isVoid() &&
                   !yield->discarded && !shape.isLoose;
  if (isUntyped) {
    auto shown = this->shownYieldedName(shape.yielded);
    auto message = ErrorTextYield::untyped(shown);

    return err(GeneratorCheckerSentDeclaredError{
        .code = GeneratorCheckerSentDeclaredErrorCode::UNTYPED_SENT,
        .message = message,
    });
  }

  return ok(shape.sent);
}

Expr *GeneratorChecker::checkYield(YieldExpr *yield) {
  yield->type = Type::error();
  auto *function = this->state.getFunctionStack().getCurrentFunction();
  bool isInsideGenerator =
      function != nullptr && function->decl != nullptr && function->decl->isGenerator;
  if (!isInsideGenerator) {
    return this->checkStrayYield(yield);
  }
  auto shape = this->yieldShape(yield, function);
  if (yield->delegate) {
    this->walkDelegate(yield, shape);
  } else {
    this->walkYieldValue(yield, shape);
  }
  if (!function->hasReturnType() && this->state.getFunctionStack().isCollectingYields()) {
    this->state.getFunctionStack().addCollectedYield(yield);
  }
  this->flowChecker->calledFunction();
  auto declared = this->checkSentDeclared(yield, shape);
  if (declared.isErr()) {
    this->state.getDiagnostics().error(yield->loc(), declared.error().message);
    return yield;
  }
  yield->type = this->yieldResultType(yield, shape);

  return yield;
}

Expr *GeneratorChecker::checkStrayYield(YieldExpr *yield) {
  if (yield->value != nullptr) {
    yield->value = this->exprChecker->checkExpr(yield->value);
  }

  return yield;
}

GeneratorChecker::YieldShape GeneratorChecker::yieldShape(const YieldExpr *yield,
                                                          FuncSymbol *function) {
  bool isAsync = function->decl->isAsync;
  const Type *known = function->hasReturnType() ? function->getKnownReturnType() : nullptr;
  bool isIterating = known != nullptr && this->isIteratorKind(known, isAsync);
  const Type *generator = isIterating ? known : nullptr;
  const Type *yielded = generator != nullptr ? generator->yielded() : nullptr;
  const Type *declaredSent = generator != nullptr ? generator->sent() : Type::voidTy();
  bool isLoose = function->hasReturnType() && generator == nullptr;
  const Type *sent =
      isAsync ? this->asyncSentType(yield, declaredSent, yielded, isLoose) : declaredSent;

  return YieldShape{
      .function = function,
      .isAsync = isAsync,
      .yielded = yielded,
      .sent = sent,
      .isLoose = isLoose,
  };
}

bool GeneratorChecker::isIteratorKind(const Type *type, bool isAsync) const {
  if (isAsync) {
    return type->isAsyncIterator();
  }

  return type->isIterator();
}

const Type *GeneratorChecker::asyncSentType(const YieldExpr *yield, const Type *sent,
                                            const Type *yielded, bool isLoose) {
  auto nothing = this->checkAsyncSent(sent, yielded, isLoose);
  if (nothing.isErr()) {
    this->state.getDiagnostics().error(yield->loc(), nothing.error().message);
    return Type::voidTy();
  }

  return sent;
}

void GeneratorChecker::walkYieldValue(YieldExpr *yield, const YieldShape &shape) {
  if (yield->value == nullptr) {
    return this->walkEmptyYield(yield, shape);
  }
  yield->value = this->exprChecker->checkExpr(yield->value, shape.yielded);
  if (shape.isAsync) {
    return this->walkAsyncYieldValue(yield, shape);
  }
  if (shape.yielded != nullptr) {
    yield->value = this->typeRelations->coerce(yield->value, shape.yielded);
  }
}

void GeneratorChecker::walkEmptyYield(const YieldExpr *yield, const YieldShape &shape) {
  auto value = this->checkValueYielded(shape);
  if (value.isErr()) {
    this->state.getDiagnostics().error(yield->loc(), value.error().message);
  }
}

void GeneratorChecker::walkAsyncYieldValue(YieldExpr *yield, const YieldShape &shape) {
  const Type *type = yield->value->type;
  const Type *given = type->isError() ? type : AsyncChecker::awaitedType(type);
  auto fits = this->checkPromiseYield(type, given, shape);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(yield->value->loc(), fits.error().message);
    return;
  }
  if (shape.yielded != nullptr && !given->isError() && given == type) {
    yield->value = this->typeRelations->coerce(yield->value, shape.yielded);
  }
}

void GeneratorChecker::walkDelegate(YieldExpr *yield, const YieldShape &shape) {
  if (!shape.isAsync) {
    this->iteratorMethods->markFused(yield->value);
  }
  auto *checked = this->exprChecker->checkExpr(yield->value);
  yield->value = this->dictChecker->iterableOf(checked);
  if (shape.isAsync) {
    return this->walkAsyncDelegate(yield, shape);
  }

  return this->walkSyncDelegate(yield, shape);
}

void GeneratorChecker::walkAsyncDelegate(YieldExpr *yield, const YieldShape &shape) {
  const Type *type = yield->value->type;
  const auto *element = this->asyncIteratedType(type);
  auto fits = this->checkDelegateAsync(type, element, shape);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(yield->value->loc(), fits.error().message);
  }
  yield->element = element != nullptr ? element : Type::error();
}

void GeneratorChecker::walkSyncDelegate(YieldExpr *yield, const YieldShape &shape) {
  const Type *type = yield->value->type;
  const auto *element = this->iteratedType(type);
  auto fits = this->checkDelegateSync(type, element, shape);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(yield->value->loc(), fits.error().message);
  }
  yield->element = element != nullptr ? element : Type::error();
}

const Type *GeneratorChecker::yieldResultType(const YieldExpr *yield,
                                              const YieldShape &shape) const {
  if (yield->delegate) {
    return this->delegateResultType(yield, shape);
  }
  if (shape.isAsync) {
    return Type::undefinedTy();
  }
  if (!shape.sent->isVoid()) {
    return shape.sent;
  }

  return Type::undefinedTy();
}

const Type *GeneratorChecker::delegateResultType(const YieldExpr *yield,
                                                 const YieldShape &shape) const {
  const Type *type = yield->value->type;
  if (type->isError()) {
    return Type::error();
  }
  bool isReturned = !shape.isAsync && type->isIterator() && !type->returned()->isVoid();

  return isReturned ? type->returned() : Type::undefinedTy();
}

Expr *GeneratorChecker::destructuredIterator(Expr *init, const VarDeclarator &declarator) {
  const Type *iterator = init->type;
  auto destructuring = ErrorTextSendsNothing::destructuring();
  auto nothing = this->checkSendsNothing(iterator, destructuring);
  if (nothing.isErr()) {
    this->state.getDiagnostics().error(init->loc(), nothing.error().message);
  }
  Expr *source = declarator.patternRest() ? init : this->takeCall(init, declarator);
  auto *array = this->state.getAst().make<ArrayLitExpr>(init->loc());
  array->elements.push_back(ArrayLitExpr::Element{.expr = source, .spread = true});
  array->type = Type::array(iterator->yielded());
  this->flowChecker->calledFunction();

  return array;
}

Expr *GeneratorChecker::takeCall(Expr *init, const VarDeclarator &declarator) {
  auto takeName = IteratorMethodTable::getName(IteratorMethod::TAKE);
  auto *member = this->state.getAst().make<MemberExpr>(init->loc(), init, takeName);
  auto *count = this->state.getAst().make<IntLitExpr>(init->loc(), declarator.patternLength());
  count->type = Type::intTy();
  auto *take = this->state.getAst().make<CallExpr>(init->loc(), member, std::vector<Expr *>{count});
  take->target = BuiltinCall{.builtin = Builtin::IterTake};
  take->fused = true;
  take->type = Type::iterator(init->type->yielded());

  return take;
}

}
