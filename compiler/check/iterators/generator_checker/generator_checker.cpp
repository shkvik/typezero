#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/iterators/iterable_checker/iterable_checker.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "llvm/ADT/STLExtras.h"


namespace tz {

GeneratorChecker::GeneratorChecker(Checker &owner)
    : checker(owner) {}

GeneratorChecker::ResultResolvedIterator GeneratorChecker::resolveIteratorType(const TypeRef &ref) {
  llvm::StringRef name = ref.name;
  if (name == "IteratorResult" && !this->checker.typeDeclarer().lookupType(name)) {
    if (ref.args.empty() || ref.args.size() > 2) {
      std::string message = ErrorTextIteratorType::resultArgs();

      return err(GeneratorCheckerResolveIteratorTypeError{
          .code = GeneratorCheckerResolveIteratorTypeErrorCode::RESULT_TYPE_ARGUMENTS,
          .message = message,
      });
    }
    const Type *value = this->checker.typeResolver().resolveType(ref.args[0], false);
    const Type *returned = Type::undefinedTy();
    if (ref.args.size() == 2) {
      const TypeRef &arg = ref.args[1];
      if (!(arg.kind() == TypeRefKind::NAME_REF &&
            (arg.name == "void" || arg.name == "undefined" || arg.name == "unknown"))) {
        returned = this->checker.typeResolver().resolveType(arg, false);
      }
    }
    if (value->isError() || returned->isError()) {
      return ok(Type::error());
    }
    const Type *result = IterableChecker::iteratorResultType(value, returned);

    return ok(result);
  }
  if (name == "Iterable" && !this->checker.typeDeclarer().lookupType(name)) {
    std::string message = ErrorTextIteratorType::iterable();

    return err(GeneratorCheckerResolveIteratorTypeError{
        .code = GeneratorCheckerResolveIteratorTypeErrorCode::ITERABLE_TYPE,
        .message = message,
    });
  }
  bool async = name == "AsyncGenerator" || name == "AsyncIterableIterator" ||
               name == "AsyncIterator" || name == "AsyncIterable";
  if (name != "Generator" && name != "IterableIterator" &&
      !(name == "Iterator" && !this->checker.typeDeclarer().lookupType(name)) && !async) {
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
  const Type *yielded = this->checker.typeResolver().resolveType(ref.args[0], false);
  auto other = [&](size_t i) -> const Type * {
    if (i >= ref.args.size()) {
      return Type::voidTy();
    }
    const TypeRef &arg = ref.args[i];
    if (arg.kind() == TypeRefKind::NAME_REF &&
        (arg.name == "void" || arg.name == "undefined" || arg.name == "unknown")) {
      return Type::voidTy();
    }

    return this->checker.typeResolver().resolveType(arg, false);
  };
  const Type *returned = other(1);
  const Type *sent = other(2);
  for (const Type *t : {yielded, returned, sent}) {
    if (t->isError()) {
      return ok(t);
    }
  }
  const Type *iterator = async ? Type::asyncIterator(yielded, returned, sent)
                               : Type::iterator(yielded, returned, sent);

  return ok(iterator);
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
    std::string name = quotedName(fn->name);
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
    std::string name = quotedName(fn->name);
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
    std::string name = quotedName(fn->name);
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
  const Type *yielded = nullptr;
  bool broken = false;
  bool async = fn->decl && fn->decl->isAsync;
  for (YieldExpr *yield : yields) {
    const Type *t = yield->delegate ? yield->element
                    : yield->value  ? yield->value->type
                                    : Type::undefinedTy();
    if (async && !yield->delegate && t && !t->isError()) {
      t = AsyncChecker::awaitedType(t);
    }
    if (!t || t->isError()) {
      broken = true;
      continue;
    }
    ResultYieldedTypesJoin joined = GeneratorChecker::yieldsJoin(yielded, t, fn);
    if (joined.isErr()) {
      if (report) {
        this->checker.diags_.error(yield->loc(), joined.error().message);
      }
      return Type::error();
    }
    yielded = joined.value();
  }
  if (!yielded || yielded->isUndefined()) {
    if (!report) {
      return nullptr;
    }
    if (broken && !yielded) {
      return Type::error();
    }
    ResultYieldedTypeKnown known = GeneratorChecker::yieldsKnown(yielded, fn, yields.empty());
    if (known.isErr()) {
      this->checker.diags_.error(fn->loc, known.error().message);
    }
    return Type::error();
  }

  const Type *returned = nullptr;
  bool undefined = fallsOff;
  for (ReturnStmt *ret : returns) {
    if (!ret->value || ret->value->type->isUndefined()) {
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
    if (!t || t->isUndefined()) {
      undefined = true;
      continue;
    }
    ResultReturnsOfOneType joined = GeneratorChecker::returnsJoin(returned, t, fn);
    if (joined.isErr()) {
      if (report) {
        this->checker.diags_.error(ret->value->loc(), joined.error().message);
      }
      return Type::error();
    }
    returned = joined.value();
  }
  if (!returned) {
    returned = Type::voidTy();
  } else if (undefined && !returned->isVoid()) {
    returned = Type::optional(returned);
  }
  if (async) {
    return Type::asyncIterator(yielded, returned, Type::voidTy());
  }

  return Type::iterator(yielded, returned, Type::voidTy());
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
    std::string name = quotedName(fn->name);
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
    std::string name = quotedName(fn->name);
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
    std::string name = quotedName(fn->name);
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
    std::string name = quotedName(fn->name);
    std::string gets = sent->isVoid() ? ErrorTextYield::nothing() : quotedName(sent->name());
    std::string message = ErrorTextYield::sends(type->name(), type->sent()->name(), name, gets);

    return err(GeneratorCheckerDelegateSyncError{
        .code = GeneratorCheckerDelegateSyncErrorCode::SENDS_DIFFER,
        .message = message,
    });
  }
  if (element && yielded && !TypeRelations::isImplicitlyConvertible(element, yielded)) {
    std::string name = quotedName(fn->name);
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
  FuncSymbol *fn = this->checker.currentFunction_;
  if (!fn || !fn->decl || !fn->decl->isGenerator) {
    if (yield->value) {
      yield->value = this->checker.exprChecker().checkExpr(yield->value);
    }
    return yield;
  }
  bool async = fn->decl->isAsync;
  const Type *generator =
      fn->ret && (async ? fn->ret->isAsyncIterator() : fn->ret->isIterator()) ? fn->ret : nullptr;
  const Type *yielded = generator ? generator->yielded() : nullptr;
  const Type *sent = generator ? generator->sent() : Type::voidTy();
  bool loose = fn->ret && !generator;
  if (async) {
    ResultNothingSentAsync nothing = GeneratorChecker::asyncSent(sent, yielded, loose);
    if (nothing.isErr()) {
      this->checker.diags_.error(yield->loc(), nothing.error().message);
      sent = Type::voidTy();
    }
  }

  if (!yield->delegate && async) {
    if (yield->value) {
      yield->value = this->checker.exprChecker().checkExpr(yield->value, yielded);
      const Type *t = yield->value->type;
      const Type *given = t->isError() ? t : AsyncChecker::awaitedType(t);
      ResultPromiseYieldFits fits = GeneratorChecker::promiseYield(t, given, yielded, fn);
      if (fits.isErr()) {
        this->checker.diags_.error(yield->value->loc(), fits.error().message);
      } else if (yielded && !given->isError() && given == t) {
        yield->value = this->checker.typeRelations().coerce(yield->value, yielded);
      }
    } else {
      ResultYieldGivesAValue value = GeneratorChecker::valueYielded(yielded, fn);
      if (value.isErr()) {
        this->checker.diags_.error(yield->loc(), value.error().message);
      }
    }
  } else if (yield->delegate && async) {
    yield->value =
        this->checker.dictChecker().iterableOf(this->checker.exprChecker().checkExpr(yield->value));
    const Type *type = yield->value->type;
    const Type *element = this->asyncIteratedType(type);
    ResultDelegateAsyncFit fits = GeneratorChecker::delegateAsync(type, element, yielded, fn);
    if (fits.isErr()) {
      this->checker.diags_.error(yield->value->loc(), fits.error().message);
    }
    yield->element = element ? element : Type::error();
  } else if (!yield->delegate) {
    if (yield->value) {
      yield->value = this->checker.exprChecker().checkExpr(yield->value, yielded);
      if (yielded) {
        yield->value = this->checker.typeRelations().coerce(yield->value, yielded);
      }
    } else {
      ResultYieldGivesAValue value = GeneratorChecker::valueYielded(yielded, fn);
      if (value.isErr()) {
        this->checker.diags_.error(yield->loc(), value.error().message);
      }
    }
  } else {
    this->checker.iteratorMethods().markFused(yield->value);
    yield->value =
        this->checker.dictChecker().iterableOf(this->checker.exprChecker().checkExpr(yield->value));
    const Type *type = yield->value->type;
    const Type *element = GeneratorChecker::iteratedType(type);
    ResultDelegateSyncFits fits =
        GeneratorChecker::delegateSync(type, element, yielded, sent, loose, fn);
    if (fits.isErr()) {
      this->checker.diags_.error(yield->value->loc(), fits.error().message);
    }
    yield->element = element ? element : Type::error();
  }
  if (!fn->ret && this->checker.yields_) {
    this->checker.yields_->push_back(yield);
  }
  this->checker.flowChecker().calledFunction();

  ResultSentTypeDeclared declared =
      GeneratorChecker::sentDeclared(yield, async, sent, yielded, loose);
  if (declared.isErr()) {
    this->checker.diags_.error(yield->loc(), declared.error().message);
    return yield;
  }
  if (yield->delegate) {
    const Type *type = yield->value->type;
    yield->type = type->isError() ? Type::error()
                  : !async && type->isIterator() && !type->returned()->isVoid()
                      ? type->returned()
                      : Type::undefinedTy();
  } else if (async) {
    yield->type = Type::undefinedTy();
  } else if (!sent->isVoid()) {
    yield->type = sent;
  } else {
    yield->type = Type::undefinedTy();
  }

  return yield;
}

Expr *GeneratorChecker::destructuredIterator(Expr *init, const VarDeclarator &d) {
  const Type *iterator = init->type;
  ResultNextNeedsNoValue nothing = this->checkSendsNothing(iterator, "destructuring");
  if (nothing.isErr()) {
    this->checker.diags_.error(init->loc(), nothing.error().message);
  }
  Expr *source = init;
  if (!d.patternRest()) {
    auto *member = this->checker.ctx_.make<MemberExpr>(init->loc(), init, "take");
    auto *n = this->checker.ctx_.make<IntLitExpr>(init->loc(), d.patternLength());
    n->type = Type::intTy();
    auto *take = this->checker.ctx_.make<CallExpr>(init->loc(), member, std::vector<Expr *>{n});
    take->target = BuiltinCall{.builtin = Builtin::IterTake};
    take->fused = true;
    take->type = Type::iterator(iterator->yielded());
    source = take;
  }
  auto *array = this->checker.ctx_.make<ArrayLitExpr>(init->loc());
  array->elements.push_back({source, true});
  array->type = Type::array(iterator->yielded());
  this->checker.flowChecker().calledFunction();

  return array;
}

}
