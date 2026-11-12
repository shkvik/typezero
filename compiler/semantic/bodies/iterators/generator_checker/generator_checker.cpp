#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
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
                                                                               bool async) {
  const auto *yielded = this->typeResolver->resolveType(ref.args[0]);
  const auto *returned = this->typeArgOr(ref, 1, Type::voidTy());
  const auto *sent = this->typeArgOr(ref, 2, Type::voidTy());
  for (const Type *type : {yielded, returned, sent}) {
    if (type->isError()) {
      return ok(type);
    }
  }
  const Type *iterator = async ? Type::asyncIterator(yielded, returned, sent)
                               : Type::iterator(yielded, returned, sent);

  return ok(iterator);
}

const Type *GeneratorChecker::typeArgOr(const TypeRef &ref, size_t index, const Type *fallback) {
  if (index >= ref.args.size()) {
    return fallback;
  }
  const TypeRef &arg = ref.args[index];
  if (this->namesNoValue(arg)) {
    return fallback;
  }

  return this->typeResolver->resolveType(arg);
}

bool GeneratorChecker::namesNoValue(const TypeRef &arg) const {
  if (arg.kind() != TypeRefKind::NAME_REF) {
    return false;
  }
  bool isVoid = TypeNameTable::isName(arg.name, TypeName::VOID_TYPE);
  bool isUndefined = TypeNameTable::isName(arg.name, TypeName::UNDEFINED);
  bool isUnknown = TypeNameTable::isName(arg.name, TypeName::UNKNOWN);

  return isVoid || isUndefined || isUnknown;
}

GeneratorChecker::ResultGeneratorReturns
GeneratorChecker::generatorReturnType(const FunctionDecl *functionDecl, const Type *declared) {
  bool isPlain = !functionDecl->isGenerator || declared->isError();
  if (!isPlain && functionDecl->isAsync && !declared->isAsyncIterator()) {
    const Type *yielded = declared->isIterator() ? declared->yielded() : declared;
    std::string shown = yielded->isVoid() ? std::string("int") : yielded->name().str();
    auto message = ErrorTextGeneratorType::async(shown);

    return err(GeneratorCheckerGeneratorReturnTypeError{
        .code = GeneratorCheckerGeneratorReturnTypeErrorCode::ASYNC_NEEDS_ASYNC_ITERATOR,
        .message = message,
    });
  }
  if (!isPlain && !functionDecl->isAsync && !declared->isIterator()) {
    std::string shown = declared->isVoid() ? std::string("int") : declared->name().str();
    auto message = ErrorTextGeneratorType::sync(shown);

    return err(GeneratorCheckerGeneratorReturnTypeError{
        .code = GeneratorCheckerGeneratorReturnTypeErrorCode::NEEDS_ITERATOR,
        .message = message,
    });
  }

  return ok(declared);
}

GeneratorChecker::ResultYieldedTypesJoin
GeneratorChecker::yieldsJoin(const Type *yielded, const Type *type,
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
GeneratorChecker::returnsJoin(const Type *returned, const Type *type,
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

GeneratorChecker::ResultYieldedTypeKnown GeneratorChecker::yieldsKnown(const Type *yielded,
                                                                       const FuncSymbol *function,
                                                                       bool noYields) const {
  if (yielded == nullptr || yielded->isUndefined()) {
    std::string why = noYields             ? ErrorTextYields::noYield()
                      : yielded != nullptr ? ErrorTextYields::onlyUndef()
                                           : "";
    auto name = ErrorTextFunctionName::quoted(function->name);
    auto message = ErrorTextYields::unknown(name, why);

    return err(GeneratorCheckerYieldsKnownError{
        .code = GeneratorCheckerYieldsKnownErrorCode::NOTHING_YIELDED,
        .message = message,
    });
  }

  return ok(yielded);
}

const Type *GeneratorChecker::inferGeneratorType(const FuncSymbol *function,
                                                 llvm::ArrayRef<YieldExpr *> yields,
                                                 llvm::ArrayRef<ReturnStmt *> returns,
                                                 bool isFallingOff, ReturnErrors errors) {
  bool isAsync = function->decl != nullptr && function->decl->isAsync;
  auto join = this->joinYields(function, yields, isAsync, errors);
  if (join.failed) {
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
                                                         bool async, ReturnErrors errors) {
  YieldJoin join{
      .yielded = nullptr,
      .broken = false,
      .failed = false,
  };
  for (YieldExpr *yield : yields) {
    const auto *type = this->yieldedValueType(yield, async);
    if (type == nullptr || type->isError()) {
      join.broken = true;
      continue;
    }
    auto joined = this->yieldsJoin(join.yielded, type, function);
    if (joined.isErr() && errors == ReturnErrors::REPORTED) {
      this->state.getDiagnostics().error(yield->loc(), joined.error().message);
    }
    if (joined.isErr()) {
      join.failed = true;
      return join;
    }
    join.yielded = joined.value();
  }

  return join;
}

const Type *GeneratorChecker::yieldedValueType(const YieldExpr *yield, bool async) const {
  const auto *type = Type::undefinedTy();
  if (yield->delegate) {
    type = yield->element;
  } else if (yield->value != nullptr) {
    type = yield->value->type;
  }
  if (async && !yield->delegate && type != nullptr && !type->isError()) {
    type = AsyncChecker::awaitedType(type);
  }

  return type;
}

const Type *GeneratorChecker::unknownYields(const FuncSymbol *function,
                                            llvm::ArrayRef<YieldExpr *> yields,
                                            const YieldJoin &join, ReturnErrors errors) {
  if (errors == ReturnErrors::IGNORED) {
    return nullptr;
  }
  if (join.broken && join.yielded == nullptr) {
    return Type::error();
  }
  auto known = this->yieldsKnown(join.yielded, function, yields.empty());
  if (known.isErr()) {
    this->state.getDiagnostics().error(function->loc, known.error().message);
  }

  return Type::error();
}

std::optional<const Type *> GeneratorChecker::joinedReturns(const FuncSymbol *function,
                                                            llvm::ArrayRef<ReturnStmt *> returns,
                                                            bool isFallingOff, bool async,
                                                            ReturnErrors errors) {
  const Type *returned = nullptr;
  bool isUndefined = isFallingOff;
  for (ReturnStmt *returnStmt : returns) {
    if (returnStmt->value == nullptr || returnStmt->value->type->isUndefined()) {
      isUndefined = true;
      continue;
    }
    const Type *type = returnStmt->value->type;
    if (type->isError()) {
      continue;
    }
    if (async) {
      type = AsyncChecker::awaitedType(type);
    }
    if (type == nullptr || type->isUndefined()) {
      isUndefined = true;
      continue;
    }
    auto joined = this->returnsJoin(returned, type, function);
    if (joined.isErr() && errors == ReturnErrors::REPORTED) {
      this->state.getDiagnostics().error(returnStmt->value->loc(), joined.error().message);
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
GeneratorChecker::asyncSent(const Type *sent, const Type *yielded, bool loose) const {
  if (!sent->isVoid() && !loose) {
    std::string shown = yielded != nullptr ? yielded->name().str() : std::string("int");
    auto message = ErrorTextYield::asyncSent(shown);

    return err(GeneratorCheckerAsyncSentError{
        .code = GeneratorCheckerAsyncSentErrorCode::SENT_TO_ASYNC,
        .message = message,
    });
  }

  return ok(sent);
}

GeneratorChecker::ResultPromiseYieldFits
GeneratorChecker::promiseYield(const Type *type, const Type *given, const Type *yielded,
                               const FuncSymbol *function) const {
  if (given == nullptr) {
    auto message = ErrorTextYield::mixed(type->name());

    return err(GeneratorCheckerPromiseYieldError{
        .code = GeneratorCheckerPromiseYieldErrorCode::MIXED_PROMISE,
        .message = message,
    });
  }
  bool isOther = yielded != nullptr && !given->isError() && given != type &&
                 !TypeRelations::isImplicitlyConvertible(given, yielded);
  if (isOther) {
    auto name = ErrorTextFunctionName::quoted(function->name);
    auto message = ErrorTextYield::promise(given->name(), name, yielded->name());

    return err(GeneratorCheckerPromiseYieldError{
        .code = GeneratorCheckerPromiseYieldErrorCode::OTHER_TYPE,
        .message = message,
    });
  }

  return ok(given);
}

GeneratorChecker::ResultYieldGivesAValue
GeneratorChecker::valueYielded(const Type *yielded, const FuncSymbol *function) const {
  if (yielded != nullptr && !yielded->isOptional() && !yielded->isError()) {
    auto name = ErrorTextFunctionName::quoted(function->name);
    auto message = ErrorTextYield::noValue(name, yielded->name());

    return err(GeneratorCheckerValueYieldedError{
        .code = GeneratorCheckerValueYieldedErrorCode::UNDEFINED_YIELD,
        .message = message,
    });
  }

  return ok(yielded);
}

GeneratorChecker::ResultDelegateAsyncFit
GeneratorChecker::delegateAsync(const Type *type, const Type *element, const Type *yielded,
                                const FuncSymbol *function) const {
  if (element == nullptr && !type->isError()) {
    auto message = ErrorTextYield::notAsync(type->name());

    return err(GeneratorCheckerDelegateAsyncError{
        .code = GeneratorCheckerDelegateAsyncErrorCode::NOT_ASYNC_ITERABLE,
        .message = message,
    });
  }
  if (element != nullptr && yielded != nullptr &&
      !TypeRelations::isImplicitlyConvertible(element, yielded)) {
    auto name = ErrorTextFunctionName::quoted(function->name);
    auto message = ErrorTextYield::otherType(element->name(), name, yielded->name());

    return err(GeneratorCheckerDelegateAsyncError{
        .code = GeneratorCheckerDelegateAsyncErrorCode::OTHER_TYPE,
        .message = message,
    });
  }

  return ok(element);
}

GeneratorChecker::ResultDelegateSyncFits
GeneratorChecker::delegateSync(const Type *type, const Type *element, const Type *yielded,
                               const Type *sent, bool loose, const FuncSymbol *function) const {
  if (element == nullptr && !type->isError()) {
    auto message = ErrorTextYield::notIter(type->name());

    return err(GeneratorCheckerDelegateSyncError{
        .code = GeneratorCheckerDelegateSyncErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }
  bool isSending = element != nullptr && type->isIterator() && !type->sent()->isVoid() &&
                   type->sent() != sent && !loose;
  if (isSending) {
    auto name = ErrorTextFunctionName::quoted(function->name);
    std::string gets =
        sent->isVoid() ? ErrorTextYield::nothing() : ErrorTextFunctionName::quoted(sent->name());
    auto message = ErrorTextYield::sends(type->name(), type->sent()->name(), name, gets);

    return err(GeneratorCheckerDelegateSyncError{
        .code = GeneratorCheckerDelegateSyncErrorCode::SENDS_DIFFER,
        .message = message,
    });
  }
  if (element != nullptr && yielded != nullptr &&
      !TypeRelations::isImplicitlyConvertible(element, yielded)) {
    auto name = ErrorTextFunctionName::quoted(function->name);
    auto message = ErrorTextYield::otherType(element->name(), name, yielded->name());

    return err(GeneratorCheckerDelegateSyncError{
        .code = GeneratorCheckerDelegateSyncErrorCode::OTHER_TYPE,
        .message = message,
    });
  }

  return ok(element);
}

GeneratorChecker::ResultSentTypeDeclared
GeneratorChecker::sentDeclared(const YieldExpr *yield, bool async, const Type *sent,
                               const Type *yielded, bool loose) const {
  bool isUntyped = !yield->delegate && !async && sent->isVoid() && !yield->discarded && !loose;
  if (isUntyped) {
    std::string shown = yielded != nullptr ? yielded->name().str() : std::string("int");
    auto message = ErrorTextYield::untyped(shown);

    return err(GeneratorCheckerSentDeclaredError{
        .code = GeneratorCheckerSentDeclaredErrorCode::UNTYPED_SENT,
        .message = message,
    });
  }

  return ok(sent);
}

Expr *GeneratorChecker::checkYield(YieldExpr *yield) {
  yield->type = Type::error();
  auto *function = this->state.getFunctionStack().getCurrentFunction();
  if (function == nullptr || function->decl == nullptr || !function->decl->isGenerator) {
    if (yield->value != nullptr) {
      yield->value = this->exprChecker->checkExpr(yield->value);
    }
    return yield;
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
  auto declared = this->sentDeclared(yield, shape.async, shape.sent, shape.yielded, shape.loose);
  if (declared.isErr()) {
    this->state.getDiagnostics().error(yield->loc(), declared.error().message);
    return yield;
  }
  yield->type = this->yieldResultType(yield, shape);

  return yield;
}

GeneratorChecker::YieldShape GeneratorChecker::yieldShape(const YieldExpr *yield,
                                                          FuncSymbol *function) {
  bool isAsync = function->decl->isAsync;
  const Type *known = function->hasReturnType() ? function->getKnownReturnType() : nullptr;
  bool isIterating = known != nullptr && (isAsync ? known->isAsyncIterator() : known->isIterator());
  const Type *generator = isIterating ? known : nullptr;
  YieldShape shape{
      .fn = function,
      .async = isAsync,
      .yielded = generator != nullptr ? generator->yielded() : nullptr,
      .sent = generator != nullptr ? generator->sent() : Type::voidTy(),
      .loose = function->hasReturnType() && generator == nullptr,
  };
  if (!isAsync) {
    return shape;
  }
  auto nothing = this->asyncSent(shape.sent, shape.yielded, shape.loose);
  if (nothing.isErr()) {
    this->state.getDiagnostics().error(yield->loc(), nothing.error().message);
    shape.sent = Type::voidTy();
  }

  return shape;
}

void GeneratorChecker::walkYieldValue(YieldExpr *yield, const YieldShape &shape) {
  if (yield->value == nullptr) {
    auto value = this->valueYielded(shape.yielded, shape.fn);
    if (value.isErr()) {
      this->state.getDiagnostics().error(yield->loc(), value.error().message);
    }
    return;
  }
  yield->value = this->exprChecker->checkExpr(yield->value, shape.yielded);
  if (shape.async) {
    return this->walkAsyncYieldValue(yield, shape);
  }
  if (shape.yielded != nullptr) {
    yield->value = this->typeRelations->coerce(yield->value, shape.yielded);
  }
}

void GeneratorChecker::walkAsyncYieldValue(YieldExpr *yield, const YieldShape &shape) {
  const Type *type = yield->value->type;
  const Type *given = type->isError() ? type : AsyncChecker::awaitedType(type);
  auto fits = this->promiseYield(type, given, shape.yielded, shape.fn);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(yield->value->loc(), fits.error().message);
    return;
  }
  if (shape.yielded != nullptr && !given->isError() && given == type) {
    yield->value = this->typeRelations->coerce(yield->value, shape.yielded);
  }
}

void GeneratorChecker::walkDelegate(YieldExpr *yield, const YieldShape &shape) {
  if (!shape.async) {
    this->iteratorMethods->markFused(yield->value);
  }
  auto *checked = this->exprChecker->checkExpr(yield->value);
  yield->value = this->dictChecker->iterableOf(checked);
  const Type *type = yield->value->type;
  if (shape.async) {
    const auto *element = this->asyncIteratedType(type);
    auto fits = this->delegateAsync(type, element, shape.yielded, shape.fn);
    if (fits.isErr()) {
      this->state.getDiagnostics().error(yield->value->loc(), fits.error().message);
    }
    yield->element = element != nullptr ? element : Type::error();
    return;
  }
  const auto *element = this->iteratedType(type);
  auto fits = this->delegateSync(type, element, shape.yielded, shape.sent, shape.loose, shape.fn);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(yield->value->loc(), fits.error().message);
  }
  yield->element = element != nullptr ? element : Type::error();
}

const Type *GeneratorChecker::yieldResultType(const YieldExpr *yield,
                                              const YieldShape &shape) const {
  if (yield->delegate) {
    const Type *type = yield->value->type;
    if (type->isError()) {
      return Type::error();
    }
    bool isReturned = !shape.async && type->isIterator() && !type->returned()->isVoid();
    return isReturned ? type->returned() : Type::undefinedTy();
  }
  if (shape.async) {
    return Type::undefinedTy();
  }
  if (!shape.sent->isVoid()) {
    return shape.sent;
  }

  return Type::undefinedTy();
}

Expr *GeneratorChecker::destructuredIterator(Expr *init, const VarDeclarator &declarator) {
  const Type *iterator = init->type;
  auto nothing = this->checkSendsNothing(iterator, "destructuring");
  if (nothing.isErr()) {
    this->state.getDiagnostics().error(init->loc(), nothing.error().message);
  }
  Expr *source = init;
  if (!declarator.patternRest()) {
    auto *member = this->state.getAst().make<MemberExpr>(init->loc(), init, "take");
    auto *n = this->state.getAst().make<IntLitExpr>(init->loc(), declarator.patternLength());
    n->type = Type::intTy();
    auto *take = this->state.getAst().make<CallExpr>(init->loc(), member, std::vector<Expr *>{n});
    take->target = BuiltinCall{.builtin = Builtin::IterTake};
    take->fused = true;
    take->type = Type::iterator(iterator->yielded());
    source = take;
  }
  auto *array = this->state.getAst().make<ArrayLitExpr>(init->loc());
  array->elements.push_back({source, true});
  array->type = Type::array(iterator->yielded());
  this->flowChecker->calledFunction();

  return array;
}

}
