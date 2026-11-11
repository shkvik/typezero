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
  this->dictChecker = &all.getDictChecker();
  this->exprChecker = &all.getExprChecker();
  this->flowChecker = &all.getFlowChecker();
  this->typeDeclarer = &all.getTypeDeclarer();
  this->typeResolver = &all.getTypeResolver();
  this->typeRelations = &all.getTypeRelations();
  this->iteratorMethods = &all.getIteratorMethods();
}

GeneratorChecker::ResultResolvedIterator GeneratorChecker::resolveIteratorType(const TypeRef &ref) {
  llvm::StringRef name = ref.name;
  if (TypeNameTable::isName(name, TypeName::ITERATOR_RESULT) &&
      this->typeDeclarer->lookupType(name) == nullptr) {
    return this->resolveIteratorResult(ref);
  }
  if (TypeNameTable::isName(name, TypeName::ITERABLE) &&
      this->typeDeclarer->lookupType(name) == nullptr) {
    std::string message = ErrorTextIteratorType::iterable();

    return err(GeneratorCheckerResolveIteratorTypeError{
        .code = GeneratorCheckerResolveIteratorTypeErrorCode::ITERABLE_TYPE,
        .message = message,
    });
  }
  const TypeNameInfo *builtin = TypeNameTable::find(name);
  const IteratorTypeName *iteratorName =
      builtin != nullptr ? builtin->form.as<IteratorTypeName>() : nullptr;
  bool shadowed = TypeNameTable::isName(name, TypeName::ITERATOR) &&
                  this->typeDeclarer->lookupType(name) != nullptr;
  if (iteratorName == nullptr || shadowed) {
    return err(GeneratorCheckerResolveIteratorTypeError{
        .code = GeneratorCheckerResolveIteratorTypeErrorCode::NOT_AN_ITERATOR,
        .message = "",
    });
  }
  if (ref.args.empty() || ref.args.size() > 3) {
    std::string message = ErrorTextIteratorType::typeArgs(name);

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
    std::string message = ErrorTextIteratorType::resultArgs();

    return err(GeneratorCheckerResolveIteratorTypeError{
        .code = GeneratorCheckerResolveIteratorTypeErrorCode::RESULT_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  const Type *value = this->typeResolver->resolveType(ref.args[0], false);
  const Type *returned = this->typeArgOr(ref, 1, Type::undefinedTy());
  if (value->isError() || returned->isError()) {
    return ok(Type::error());
  }
  const Type *result = IterableChecker::iteratorResultType(value, returned);

  return ok(result);
}

GeneratorChecker::ResultResolvedIterator GeneratorChecker::resolveIteratorArgs(const TypeRef &ref,
                                                                               bool async) {
  const Type *yielded = this->typeResolver->resolveType(ref.args[0], false);
  const Type *returned = this->typeArgOr(ref, 1, Type::voidTy());
  const Type *sent = this->typeArgOr(ref, 2, Type::voidTy());
  for (const Type *t : {yielded, returned, sent}) {
    if (t->isError()) {
      return ok(t);
    }
  }
  const Type *iterator = async ? Type::asyncIterator(yielded, returned, sent)
                               : Type::iterator(yielded, returned, sent);

  return ok(iterator);
}

const Type *GeneratorChecker::typeArgOr(const TypeRef &ref, size_t i, const Type *fallback) {
  if (i >= ref.args.size()) {
    return fallback;
  }
  const TypeRef &arg = ref.args[i];
  if (GeneratorChecker::namesNoValue(arg)) {
    return fallback;
  }

  return this->typeResolver->resolveType(arg, false);
}

bool GeneratorChecker::namesNoValue(const TypeRef &arg) {
  if (arg.kind() != TypeRefKind::NAME_REF) {
    return false;
  }
  bool isVoid = TypeNameTable::isName(arg.name, TypeName::VOID_TYPE);
  bool isUndefined = TypeNameTable::isName(arg.name, TypeName::UNDEFINED);
  bool isUnknown = TypeNameTable::isName(arg.name, TypeName::UNKNOWN);

  return isVoid || isUndefined || isUnknown;
}

GeneratorChecker::ResultGeneratorReturns
GeneratorChecker::generatorReturnType(const FunctionDecl *fn, const Type *declared) {
  bool plain = !fn->isGenerator || declared->isError();
  if (!plain && fn->isAsync && !declared->isAsyncIterator()) {
    const Type *yielded = declared->isIterator() ? declared->yielded() : declared;
    std::string shown = yielded->isVoid() ? std::string("int") : yielded->name().str();
    std::string message = ErrorTextGeneratorType::async(shown);

    return err(GeneratorCheckerGeneratorReturnTypeError{
        .code = GeneratorCheckerGeneratorReturnTypeErrorCode::ASYNC_NEEDS_ASYNC_ITERATOR,
        .message = message,
    });
  }
  if (!plain && !fn->isAsync && !declared->isIterator()) {
    std::string shown = declared->isVoid() ? std::string("int") : declared->name().str();
    std::string message = ErrorTextGeneratorType::sync(shown);

    return err(GeneratorCheckerGeneratorReturnTypeError{
        .code = GeneratorCheckerGeneratorReturnTypeErrorCode::NEEDS_ITERATOR,
        .message = message,
    });
  }

  return ok(declared);
}

GeneratorChecker::ResultYieldedTypesJoin
GeneratorChecker::yieldsJoin(const Type *yielded, const Type *t, const FuncSymbol *fn) {
  const Type *common = yielded ? LiteralTypes::unionUnify(yielded, t) : t;
  if (!common || common->isError()) {
    std::string name = ErrorTextFunctionName::quoted(fn->name);
    std::string message = ErrorTextYields::differ(name, t->name(), yielded->name());

    return err(GeneratorCheckerYieldsJoinError{
        .code = GeneratorCheckerYieldsJoinErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(common);
}

GeneratorChecker::ResultReturnsOfOneType
GeneratorChecker::returnsJoin(const Type *returned, const Type *t, const FuncSymbol *fn) {
  const Type *common = returned ? LiteralTypes::unionUnify(returned, t) : t;
  if (!common || common->isError()) {
    std::string name = ErrorTextFunctionName::quoted(fn->name);
    std::string message = ErrorTextAsyncReturns::differ(name, t->name(), returned->name());

    return err(GeneratorCheckerReturnsJoinError{
        .code = GeneratorCheckerReturnsJoinErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(common);
}

GeneratorChecker::ResultYieldedTypeKnown
GeneratorChecker::yieldsKnown(const Type *yielded, const FuncSymbol *fn, bool noYields) {
  if (!yielded || yielded->isUndefined()) {
    std::string why = noYields  ? ErrorTextYields::noYield()
                      : yielded ? ErrorTextYields::onlyUndef()
                                : "";
    std::string name = ErrorTextFunctionName::quoted(fn->name);
    std::string message = ErrorTextYields::unknown(name, why);

    return err(GeneratorCheckerYieldsKnownError{
        .code = GeneratorCheckerYieldsKnownErrorCode::NOTHING_YIELDED,
        .message = message,
    });
  }

  return ok(yielded);
}

const Type *GeneratorChecker::inferGeneratorType(const FuncSymbol *fn,
                                                 llvm::ArrayRef<YieldExpr *> yields,
                                                 llvm::ArrayRef<ReturnStmt *> returns,
                                                 bool fallsOff, bool report) {
  bool async = fn->decl != nullptr && fn->decl->isAsync;
  YieldJoin join = this->joinYields(fn, yields, async, report);
  if (join.failed) {
    return Type::error();
  }
  if (join.yielded == nullptr || join.yielded->isUndefined()) {
    return this->unknownYields(fn, yields, join, report);
  }
  std::optional<const Type *> returned = this->joinedReturns(fn, returns, fallsOff, async, report);
  if (!returned.has_value()) {
    return Type::error();
  }
  if (async) {
    return Type::asyncIterator(join.yielded, *returned, Type::voidTy());
  }

  return Type::iterator(join.yielded, *returned, Type::voidTy());
}

GeneratorChecker::YieldJoin GeneratorChecker::joinYields(const FuncSymbol *fn,
                                                         llvm::ArrayRef<YieldExpr *> yields,
                                                         bool async, bool report) {
  YieldJoin join{
      .yielded = nullptr,
      .broken = false,
      .failed = false,
  };
  for (YieldExpr *yield : yields) {
    const Type *t = GeneratorChecker::yieldedValueType(yield, async);
    if (t == nullptr || t->isError()) {
      join.broken = true;
      continue;
    }
    ResultYieldedTypesJoin joined = GeneratorChecker::yieldsJoin(join.yielded, t, fn);
    if (joined.isErr()) {
      if (report) {
        this->state.getDiagnostics().error(yield->loc(), joined.error().message);
      }
      join.failed = true;
      return join;
    }
    join.yielded = joined.value();
  }

  return join;
}

const Type *GeneratorChecker::yieldedValueType(const YieldExpr *yield, bool async) {
  const Type *t = Type::undefinedTy();
  if (yield->delegate) {
    t = yield->element;
  } else if (yield->value != nullptr) {
    t = yield->value->type;
  }
  if (async && !yield->delegate && t != nullptr && !t->isError()) {
    t = AsyncChecker::awaitedType(t);
  }

  return t;
}

const Type *GeneratorChecker::unknownYields(const FuncSymbol *fn,
                                            llvm::ArrayRef<YieldExpr *> yields,
                                            const YieldJoin &join, bool report) {
  if (!report) {
    return nullptr;
  }
  if (join.broken && join.yielded == nullptr) {
    return Type::error();
  }
  ResultYieldedTypeKnown known = GeneratorChecker::yieldsKnown(join.yielded, fn, yields.empty());
  if (known.isErr()) {
    this->state.getDiagnostics().error(fn->loc, known.error().message);
  }

  return Type::error();
}

std::optional<const Type *> GeneratorChecker::joinedReturns(const FuncSymbol *fn,
                                                            llvm::ArrayRef<ReturnStmt *> returns,
                                                            bool fallsOff, bool async,
                                                            bool report) {
  const Type *returned = nullptr;
  bool undefined = fallsOff;
  for (ReturnStmt *ret : returns) {
    if (ret->value == nullptr || ret->value->type->isUndefined()) {
      undefined = true;
      continue;
    }
    const Type *t = ret->value->type;
    if (t->isError()) {
      continue;
    }
    if (async) {
      t = AsyncChecker::awaitedType(t);
    }
    if (t == nullptr || t->isUndefined()) {
      undefined = true;
      continue;
    }
    ResultReturnsOfOneType joined = GeneratorChecker::returnsJoin(returned, t, fn);
    if (joined.isErr()) {
      if (report) {
        this->state.getDiagnostics().error(ret->value->loc(), joined.error().message);
      }
      return std::nullopt;
    }
    returned = joined.value();
  }
  if (returned == nullptr) {
    return Type::voidTy();
  }
  if (undefined && !returned->isVoid()) {
    return Type::optional(returned);
  }

  return returned;
}

const Type *GeneratorChecker::iteratedType(const Type *type) {
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
  if (type->isParam() && type->iterated()) {
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
  const Type *element = GeneratorChecker::iteratedType(type);
  if (!element) {
    return nullptr;
  }
  const Type *awaited = AsyncChecker::awaitedType(element);

  return awaited && awaited->isVoid() ? Type::undefinedTy() : awaited;
}

GeneratorChecker::ResultNextNeedsNoValue GeneratorChecker::checkSendsNothing(const Type *iterator,
                                                                             llvm::StringRef what) {
  const Type *sent = iterator->sent();
  bool free = sent->isVoid() || sent->isOptional() || sent->isError();
  if (!free) {
    std::string message = ErrorTextSendsNothing::expects(what, iterator->name(), sent->name());

    return err(GeneratorCheckerCheckSendsNothingError{
        .code = GeneratorCheckerCheckSendsNothingErrorCode::EXPECTS_VALUE,
        .message = message,
    });
  }

  return ok(iterator);
}

GeneratorChecker::ResultNothingSentAsync
GeneratorChecker::asyncSent(const Type *sent, const Type *yielded, bool loose) {
  if (!sent->isVoid() && !loose) {
    std::string shown = yielded ? yielded->name().str() : std::string("int");
    std::string message = ErrorTextYield::asyncSent(shown);

    return err(GeneratorCheckerAsyncSentError{
        .code = GeneratorCheckerAsyncSentErrorCode::SENT_TO_ASYNC,
        .message = message,
    });
  }

  return ok(sent);
}

GeneratorChecker::ResultPromiseYieldFits GeneratorChecker::promiseYield(const Type *t,
                                                                        const Type *given,
                                                                        const Type *yielded,
                                                                        const FuncSymbol *fn) {
  if (!given) {
    std::string message = ErrorTextYield::mixed(t->name());

    return err(GeneratorCheckerPromiseYieldError{
        .code = GeneratorCheckerPromiseYieldErrorCode::MIXED_PROMISE,
        .message = message,
    });
  }
  bool other = yielded && !given->isError() && given != t &&
               !TypeRelations::isImplicitlyConvertible(given, yielded);
  if (other) {
    std::string name = ErrorTextFunctionName::quoted(fn->name);
    std::string message = ErrorTextYield::promise(given->name(), name, yielded->name());

    return err(GeneratorCheckerPromiseYieldError{
        .code = GeneratorCheckerPromiseYieldErrorCode::OTHER_TYPE,
        .message = message,
    });
  }

  return ok(given);
}

GeneratorChecker::ResultYieldGivesAValue GeneratorChecker::valueYielded(const Type *yielded,
                                                                        const FuncSymbol *fn) {
  if (yielded && !yielded->isOptional() && !yielded->isError()) {
    std::string name = ErrorTextFunctionName::quoted(fn->name);
    std::string message = ErrorTextYield::noValue(name, yielded->name());

    return err(GeneratorCheckerValueYieldedError{
        .code = GeneratorCheckerValueYieldedErrorCode::UNDEFINED_YIELD,
        .message = message,
    });
  }

  return ok(yielded);
}

GeneratorChecker::ResultDelegateAsyncFit GeneratorChecker::delegateAsync(const Type *type,
                                                                         const Type *element,
                                                                         const Type *yielded,
                                                                         const FuncSymbol *fn) {
  if (!element && !type->isError()) {
    std::string message = ErrorTextYield::notAsync(type->name());

    return err(GeneratorCheckerDelegateAsyncError{
        .code = GeneratorCheckerDelegateAsyncErrorCode::NOT_ASYNC_ITERABLE,
        .message = message,
    });
  }
  if (element && yielded && !TypeRelations::isImplicitlyConvertible(element, yielded)) {
    std::string name = ErrorTextFunctionName::quoted(fn->name);
    std::string message = ErrorTextYield::otherType(element->name(), name, yielded->name());

    return err(GeneratorCheckerDelegateAsyncError{
        .code = GeneratorCheckerDelegateAsyncErrorCode::OTHER_TYPE,
        .message = message,
    });
  }

  return ok(element);
}

GeneratorChecker::ResultDelegateSyncFits
GeneratorChecker::delegateSync(const Type *type, const Type *element, const Type *yielded,
                               const Type *sent, bool loose, const FuncSymbol *fn) {
  if (!element && !type->isError()) {
    std::string message = ErrorTextYield::notIter(type->name());

    return err(GeneratorCheckerDelegateSyncError{
        .code = GeneratorCheckerDelegateSyncErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }
  bool sends =
      element && type->isIterator() && !type->sent()->isVoid() && type->sent() != sent && !loose;
  if (sends) {
    std::string name = ErrorTextFunctionName::quoted(fn->name);
    std::string gets =
        sent->isVoid() ? ErrorTextYield::nothing() : ErrorTextFunctionName::quoted(sent->name());
    std::string message = ErrorTextYield::sends(type->name(), type->sent()->name(), name, gets);

    return err(GeneratorCheckerDelegateSyncError{
        .code = GeneratorCheckerDelegateSyncErrorCode::SENDS_DIFFER,
        .message = message,
    });
  }
  if (element && yielded && !TypeRelations::isImplicitlyConvertible(element, yielded)) {
    std::string name = ErrorTextFunctionName::quoted(fn->name);
    std::string message = ErrorTextYield::otherType(element->name(), name, yielded->name());

    return err(GeneratorCheckerDelegateSyncError{
        .code = GeneratorCheckerDelegateSyncErrorCode::OTHER_TYPE,
        .message = message,
    });
  }

  return ok(element);
}

GeneratorChecker::ResultSentTypeDeclared
GeneratorChecker::sentDeclared(const YieldExpr *yield, bool async, const Type *sent,
                               const Type *yielded, bool loose) {
  bool untyped = !yield->delegate && !async && sent->isVoid() && !yield->discarded && !loose;
  if (untyped) {
    std::string shown = yielded ? yielded->name().str() : std::string("int");
    std::string message = ErrorTextYield::untyped(shown);

    return err(GeneratorCheckerSentDeclaredError{
        .code = GeneratorCheckerSentDeclaredErrorCode::UNTYPED_SENT,
        .message = message,
    });
  }

  return ok(sent);
}

Expr *GeneratorChecker::checkYield(YieldExpr *yield) {
  yield->type = Type::error();
  FuncSymbol *fn = this->state.getFunctionStack().getCurrentFunction();
  if (fn == nullptr || fn->decl == nullptr || !fn->decl->isGenerator) {
    if (yield->value != nullptr) {
      yield->value = this->exprChecker->checkExpr(yield->value);
    }
    return yield;
  }
  YieldShape shape = this->yieldShape(yield, fn);
  if (yield->delegate) {
    this->walkDelegate(yield, shape);
  } else {
    this->walkYieldValue(yield, shape);
  }
  if (!fn->hasReturnType() && this->state.getFunctionStack().isCollectingYields()) {
    this->state.getFunctionStack().addCollectedYield(yield);
  }
  this->flowChecker->calledFunction();
  ResultSentTypeDeclared declared =
      GeneratorChecker::sentDeclared(yield, shape.async, shape.sent, shape.yielded, shape.loose);
  if (declared.isErr()) {
    this->state.getDiagnostics().error(yield->loc(), declared.error().message);
    return yield;
  }
  yield->type = GeneratorChecker::yieldResultType(yield, shape);

  return yield;
}

GeneratorChecker::YieldShape GeneratorChecker::yieldShape(const YieldExpr *yield, FuncSymbol *fn) {
  bool async = fn->decl->isAsync;
  const Type *known = fn->hasReturnType() ? fn->getKnownReturnType() : nullptr;
  bool iterates = known != nullptr && (async ? known->isAsyncIterator() : known->isIterator());
  const Type *generator = iterates ? known : nullptr;
  YieldShape shape{
      .fn = fn,
      .async = async,
      .yielded = generator != nullptr ? generator->yielded() : nullptr,
      .sent = generator != nullptr ? generator->sent() : Type::voidTy(),
      .loose = fn->hasReturnType() && generator == nullptr,
  };
  if (!async) {
    return shape;
  }
  ResultNothingSentAsync nothing =
      GeneratorChecker::asyncSent(shape.sent, shape.yielded, shape.loose);
  if (nothing.isErr()) {
    this->state.getDiagnostics().error(yield->loc(), nothing.error().message);
    shape.sent = Type::voidTy();
  }

  return shape;
}

void GeneratorChecker::walkYieldValue(YieldExpr *yield, const YieldShape &shape) {
  if (yield->value == nullptr) {
    ResultYieldGivesAValue value = GeneratorChecker::valueYielded(shape.yielded, shape.fn);
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
  const Type *t = yield->value->type;
  const Type *given = t->isError() ? t : AsyncChecker::awaitedType(t);
  ResultPromiseYieldFits fits = GeneratorChecker::promiseYield(t, given, shape.yielded, shape.fn);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(yield->value->loc(), fits.error().message);
    return;
  }
  if (shape.yielded != nullptr && !given->isError() && given == t) {
    yield->value = this->typeRelations->coerce(yield->value, shape.yielded);
  }
}

void GeneratorChecker::walkDelegate(YieldExpr *yield, const YieldShape &shape) {
  if (!shape.async) {
    this->iteratorMethods->markFused(yield->value);
  }
  Expr *checked = this->exprChecker->checkExpr(yield->value);
  yield->value = this->dictChecker->iterableOf(checked);
  const Type *type = yield->value->type;
  if (shape.async) {
    const Type *element = this->asyncIteratedType(type);
    ResultDelegateAsyncFit fits =
        GeneratorChecker::delegateAsync(type, element, shape.yielded, shape.fn);
    if (fits.isErr()) {
      this->state.getDiagnostics().error(yield->value->loc(), fits.error().message);
    }
    yield->element = element != nullptr ? element : Type::error();
    return;
  }
  const Type *element = GeneratorChecker::iteratedType(type);
  ResultDelegateSyncFits fits = GeneratorChecker::delegateSync(type, element, shape.yielded,
                                                               shape.sent, shape.loose, shape.fn);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(yield->value->loc(), fits.error().message);
  }
  yield->element = element != nullptr ? element : Type::error();
}

const Type *GeneratorChecker::yieldResultType(const YieldExpr *yield, const YieldShape &shape) {
  if (yield->delegate) {
    const Type *type = yield->value->type;
    if (type->isError()) {
      return Type::error();
    }
    bool returned = !shape.async && type->isIterator() && !type->returned()->isVoid();
    return returned ? type->returned() : Type::undefinedTy();
  }
  if (shape.async) {
    return Type::undefinedTy();
  }
  if (!shape.sent->isVoid()) {
    return shape.sent;
  }

  return Type::undefinedTy();
}

Expr *GeneratorChecker::destructuredIterator(Expr *init, const VarDeclarator &d) {
  const Type *iterator = init->type;
  ResultNextNeedsNoValue nothing = this->checkSendsNothing(iterator, "destructuring");
  if (nothing.isErr()) {
    this->state.getDiagnostics().error(init->loc(), nothing.error().message);
  }
  Expr *source = init;
  if (!d.patternRest()) {
    auto *member = this->state.getAst().make<MemberExpr>(init->loc(), init, "take");
    auto *n = this->state.getAst().make<IntLitExpr>(init->loc(), d.patternLength());
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
