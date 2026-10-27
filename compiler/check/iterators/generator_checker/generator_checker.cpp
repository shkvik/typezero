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

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

GeneratorChecker::GeneratorChecker(Checker &checker) : checker_(checker) {}

const Type *GeneratorChecker::resolveIteratorType(const TypeRef &ref) {
  llvm::StringRef name = ref.name;
  if (name == "IteratorResult" && !checker_.typeDeclarer().lookupType(name)) {
    if (ref.args.empty() || ref.args.size() > 2) {
      checker_.diags_.error(
          ref.loc, "'IteratorResult' takes 1-2 type arguments: the values and what the "
                   "iterator gives when it is done, e.g. 'IteratorResult<int, undefined>'");
      return Type::error();
    }
    const Type *value = checker_.typeResolver().resolveType(ref.args[0], false);
    const Type *returned = Type::undefinedTy();
    if (ref.args.size() == 2) {
      const TypeRef &arg = ref.args[1];
      if (!(arg.kind == TypeRef::Kind::Name &&
            (arg.name == "void" || arg.name == "undefined" || arg.name == "unknown"))) {
        returned = checker_.typeResolver().resolveType(arg, false);
      }
    }
    if (value->isError() || returned->isError()) {
      return Type::error();
    }
    return IterableChecker::iteratorResultType(value, returned);
  }
  if (name == "Iterable" && !checker_.typeDeclarer().lookupType(name)) {
    checker_.diags_.error(
        ref.loc, "'Iterable<T>' can only be the type of a parameter of a function declared "
                 "at the top level or of a method, which it makes generic: a copy for "
                 "each kind of iterable; here, write the type itself, e.g. 'int[]', or "
                 "'Iterator<int>' for any iterator");
    return Type::error();
  }
  bool async = name == "AsyncGenerator" || name == "AsyncIterableIterator" ||
               name == "AsyncIterator" || name == "AsyncIterable";
  if (name != "Generator" && name != "IterableIterator" &&
      !(name == "Iterator" && !checker_.typeDeclarer().lookupType(name)) && !async) {
    return nullptr;
  }
  if (ref.args.empty() || ref.args.size() > 3) {
    checker_.diags_.error(ref.loc, "'" + name +
                                       "' takes 1-3 type arguments: the values it gives, what it "
                                       "returns and what next() sends, e.g. '" +
                                       name + "<int>'");
    return Type::error();
  }
  const Type *yielded = checker_.typeResolver().resolveType(ref.args[0], false);
  auto other = [&](size_t i) -> const Type * {
    if (i >= ref.args.size()) {
      return Type::voidTy();
    }
    const TypeRef &arg = ref.args[i];
    if (arg.kind == TypeRef::Kind::Name &&
        (arg.name == "void" || arg.name == "undefined" || arg.name == "unknown")) {
      return Type::voidTy();
    }

    return checker_.typeResolver().resolveType(arg, false);
  };
  const Type *returned = other(1), *sent = other(2);
  for (const Type *t : {yielded, returned, sent}) {
    if (t->isError()) {
      return t;
    }
  }
  if (async) {
    return Type::asyncIterator(yielded, returned, sent);
  }

  return Type::iterator(yielded, returned, sent);
}

const Type *GeneratorChecker::generatorReturnType(const FunctionDecl *fn, const Type *declared) {
  if (!fn->isGenerator || declared->isError()) {
    return declared;
  }
  if (fn->isAsync) {
    if (declared->isAsyncIterator()) {
      return declared;
    }
    const Type *yielded = declared->isIterator() ? declared->yielded() : declared;
    checker_.diags_.error(fn->returnType->loc,
                          "an async generator returns an async iterator: declare its type as "
                          "'AsyncGenerator<" +
                              (yielded->isVoid() ? std::string("int") : yielded->name().str()) +
                              ">'");
    return Type::error();
  }
  if (declared->isIterator()) {
    return declared;
  }
  checker_.diags_.error(fn->returnType->loc,
                        "a generator returns an iterator: declare its type as 'Generator<" +
                            (declared->isVoid() ? std::string("int") : declared->name().str()) +
                            ">'");

  return Type::error();
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
    const Type *common = yielded ? LiteralTypes::unionUnify(yielded, t) : t;
    if (!common || common->isError()) {
      if (report) {
        checker_.diags_.error(yield->loc(), quotedName(fn->name) + " yields '" + t->name() +
                                                "' here and '" + yielded->name() +
                                                "' elsewhere; a generator yields values of "
                                                "one type");
      }
      return Type::error();
    }
    yielded = common;
  }
  if (!yielded || yielded->isUndefined()) {
    if (!report) {
      return nullptr;
    }
    if (broken && !yielded) {
      return Type::error();
    }
    std::string why = yields.empty() ? ": it has no 'yield'"
                      : yielded      ? ": it yields only undefined"
                                     : "";
    checker_.diags_.error(fn->loc, "cannot tell what " + quotedName(fn->name) + " yields" + why +
                                       "; declare its type, e.g. ': Generator<int>'");
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
    const Type *common = returned ? LiteralTypes::unionUnify(returned, t) : t;
    if (!common || common->isError()) {
      if (report) {
        checker_.diags_.error(ret->value->loc(), quotedName(fn->name) + " returns '" + t->name() +
                                                     "' here and '" + returned->name() +
                                                     "' elsewhere; a function returns values "
                                                     "of one type");
      }
      return Type::error();
    }
    returned = common;
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
  const Type *element = iteratedType(type);
  if (!element) {
    return nullptr;
  }
  const Type *awaited = AsyncChecker::awaitedType(element);

  return awaited && awaited->isVoid() ? Type::undefinedTy() : awaited;
}

bool GeneratorChecker::checkSendsNothing(const Type *iterator, llvm::SMLoc loc,
                                         llvm::StringRef what) {
  const Type *sent = iterator->sent();
  if (sent->isVoid() || sent->isOptional() || sent->isError()) {
    return true;
  }
  checker_.diags_.error(loc,
                        what + " resumes each 'yield' of '" + iterator->name() +
                            "' without a value, but it expects '" + sent->name() +
                            "' from next(x); call next(x) yourself, or let the generator take '" +
                            sent->name() + " | undefined'");

  return false;
}

Expr *GeneratorChecker::checkYield(YieldExpr *yield) {
  yield->type = Type::error();
  FuncSymbol *fn = checker_.currentFunction_;
  if (!fn || !fn->decl || !fn->decl->isGenerator) {
    if (yield->value) {
      yield->value = checker_.exprChecker().checkExpr(yield->value);
    }
    return yield;
  }
  bool async = fn->decl->isAsync;
  const Type *generator =
      fn->ret && (async ? fn->ret->isAsyncIterator() : fn->ret->isIterator()) ? fn->ret : nullptr;
  const Type *yielded = generator ? generator->yielded() : nullptr;
  const Type *sent = generator ? generator->sent() : Type::voidTy();
  bool loose = fn->ret && !generator;
  if (async && !sent->isVoid() && !loose) {
    checker_.diags_.error(yield->loc(), "values sent with next(x) to an async generator are not "
                                        "supported yet; declare it as 'AsyncGenerator<" +
                                            (yielded ? yielded->name().str() : std::string("int")) +
                                            ">'");
    sent = Type::voidTy();
  }

  if (!yield->delegate && async) {
    if (yield->value) {
      yield->value = checker_.exprChecker().checkExpr(yield->value, yielded);
      const Type *t = yield->value->type;
      const Type *given = t->isError() ? t : AsyncChecker::awaitedType(t);
      if (!given) {
        checker_.diags_.error(yield->value->loc(),
                              "'yield' of '" + t->name() +
                                  "' gives the value of the Promise or the value "
                                  "itself, which make no one type; check what it "
                                  "holds first");
      } else if (yielded && !given->isError() && given == t) {
        yield->value = checker_.typeRelations().coerce(yield->value, yielded);
      } else if (yielded && !given->isError() &&
                 !TypeRelations::isImplicitlyConvertible(given, yielded)) {
        checker_.diags_.error(yield->value->loc(), "the Promise gives '" + given->name() +
                                                       "', but " + quotedName(fn->name) +
                                                       " yields '" + yielded->name() + "'");
      }
    } else if (yielded && !yielded->isOptional() && !yielded->isError()) {
      checker_.diags_.error(yield->loc(), "'yield' without a value gives undefined, but " +
                                              quotedName(fn->name) + " yields '" + yielded->name() +
                                              "'");
    }
  } else if (yield->delegate && async) {
    yield->value =
        checker_.dictChecker().iterableOf(checker_.exprChecker().checkExpr(yield->value));
    const Type *type = yield->value->type;
    const Type *element = asyncIteratedType(type);
    if (!element) {
      if (!type->isError()) {
        checker_.diags_.error(yield->value->loc(), "'yield*' in an async generator needs an async "
                                                   "iterator, an array or an iterator, found '" +
                                                       type->name() + "'");
      }
    } else if (yielded && !TypeRelations::isImplicitlyConvertible(element, yielded)) {
      checker_.diags_.error(yield->value->loc(),
                            "'yield*' gives values of type '" + element->name() + "', but " +
                                quotedName(fn->name) + " yields '" + yielded->name() + "'");
    }
    yield->element = element ? element : Type::error();
  } else if (!yield->delegate) {
    if (yield->value) {
      yield->value = checker_.exprChecker().checkExpr(yield->value, yielded);
      if (yielded) {
        yield->value = checker_.typeRelations().coerce(yield->value, yielded);
      }
    } else if (yielded && !yielded->isOptional() && !yielded->isError()) {
      checker_.diags_.error(yield->loc(), "'yield' without a value gives undefined, but " +
                                              quotedName(fn->name) + " yields '" + yielded->name() +
                                              "'");
    }
  } else {
    checker_.iteratorMethods().markFused(yield->value);
    yield->value =
        checker_.dictChecker().iterableOf(checker_.exprChecker().checkExpr(yield->value));
    const Type *type = yield->value->type;
    const Type *element = iteratedType(type);
    if (!element) {
      if (!type->isError()) {
        checker_.diags_.error(yield->value->loc(),
                              "'yield*' needs an array, a string or an iterator, found '" +
                                  type->name() + "'");
      }
    } else if (type->isIterator() && !type->sent()->isVoid() && type->sent() != sent && !loose) {
      checker_.diags_.error(yield->value->loc(),
                            "'yield*' passes on what next(x) sends, and '" + type->name() +
                                "' expects '" + type->sent()->name() + "' from it, but " +
                                quotedName(fn->name) + " gets " +
                                (sent->isVoid() ? "nothing" : "'" + sent->name().str() + "'"));
    } else if (yielded && !TypeRelations::isImplicitlyConvertible(element, yielded)) {
      checker_.diags_.error(yield->value->loc(),
                            "'yield*' gives values of type '" + element->name() + "', but " +
                                quotedName(fn->name) + " yields '" + yielded->name() + "'");
    }
    yield->element = element ? element : Type::error();
  }
  if (!fn->ret && checker_.yields_) {
    checker_.yields_->push_back(yield);
  }
  checker_.flowChecker().calledFunction();

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
  } else if (yield->discarded || loose) {
    yield->type = Type::undefinedTy();
  } else {
    checker_.diags_.error(yield->loc(),
                          "the value of 'yield' is what next(x) sends, and nothing says its type; "
                          "declare it in the return type, e.g. ': Generator<" +
                              (yielded ? yielded->name().str() : std::string("int")) +
                              ", void, string>'");
  }

  return yield;
}

Expr *GeneratorChecker::destructuredIterator(Expr *init, const VarDeclarator &d) {
  const Type *iterator = init->type;
  checkSendsNothing(iterator, init->loc(), "destructuring");
  Expr *source = init;
  if (!d.patternRest) {
    auto *member = checker_.ctx_.make<MemberExpr>(init->loc(), init, "take");
    auto *n = checker_.ctx_.make<IntLitExpr>(init->loc(), d.patternLength);
    n->type = Type::intTy();
    auto *take = checker_.ctx_.make<CallExpr>(init->loc(), member, std::vector<Expr *>{n});
    take->builtin = Builtin::IterTake;
    take->fused = true;
    take->type = Type::iterator(iterator->yielded());
    source = take;
  }
  auto *array = checker_.ctx_.make<ArrayLitExpr>(init->loc());
  array->elements.push_back({source, true});
  array->type = Type::array(iterator->yielded());
  checker_.flowChecker().calledFunction();

  return array;
}

}
