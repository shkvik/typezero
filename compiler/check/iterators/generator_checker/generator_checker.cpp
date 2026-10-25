#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/iterators/iterable_checker/iterable_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/library/result/safe_try_checker/safe_try_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

GeneratorChecker::GeneratorChecker(Sema &sema) : sema_(sema) {}

const Type *GeneratorChecker::resolveIteratorType(const TypeRef &ref) {
  llvm::StringRef name = ref.name;
  // `IteratorResult<T, R>`, what next() of an iterator written by hand
  // gives.
  if (name == "IteratorResult" && !sema_.typeDeclarer().lookupType(name)) {
    if (ref.args.empty() || ref.args.size() > 2) {
      sema_.diags_.error(ref.loc,
                   "'IteratorResult' takes 1-2 type arguments: the values and what the "
                   "iterator gives when it is done, e.g. 'IteratorResult<int, undefined>'");
      return Type::error();
    }
    const Type *value = sema_.typeResolver().resolveType(ref.args[0], /*allowVoid=*/false);
    const Type *returned = Type::undefinedTy();
    if (ref.args.size() == 2) {
      const TypeRef &arg = ref.args[1];
      if (!(arg.kind == TypeRef::Kind::Name &&
            (arg.name == "void" || arg.name == "undefined" || arg.name == "unknown"))) {
        returned = sema_.typeResolver().resolveType(arg, /*allowVoid=*/false);
      }
    }
    if (value->isError() || returned->isError()) {
      return Type::error();
    }
    return IterableChecker::iteratorResultType(value, returned);
  }
  // `Iterable<T>` makes a function generic, as the type of a parameter (see
  // iterableParam); anywhere else it is no one type.
  if (name == "Iterable" && !sema_.typeDeclarer().lookupType(name)) {
    sema_.diags_.error(ref.loc,
                 "'Iterable<T>' can only be the type of a parameter of a function declared "
                 "at the top level or of a method, which it makes generic: a copy for "
                 "each kind of iterable; here, write the type itself, e.g. 'int[]', or "
                 "'Iterator<int>' for any iterator");
    return Type::error();
  }
  // `Iterator<T>` is any iterator, as `IterableIterator<T>` is; an iterator
  // written by hand becomes one where it is expected (see coerce). What an
  // async generator gives (docs/iterators.md, step 4) goes by the names
  // TypeScript has for it.
  bool async = name == "AsyncGenerator" || name == "AsyncIterableIterator" ||
               name == "AsyncIterator" || name == "AsyncIterable";
  if (name != "Generator" && name != "IterableIterator" &&
      !(name == "Iterator" && !sema_.typeDeclarer().lookupType(name)) && !async) {
    return nullptr;
  }
  if (ref.args.empty() || ref.args.size() > 3) {
    sema_.diags_.error(ref.loc, "'" + name +
                              "' takes 1-3 type arguments: the values it gives, what it "
                              "returns and what next() sends, e.g. '" +
                              name + "<int>'");
    return Type::error();
  }
  const Type *yielded = sema_.typeResolver().resolveType(ref.args[0], /*allowVoid=*/false);
  // What it returns and what next() sends: void for nothing, which
  // undefined and TypeScript's unknown also say.
  auto other = [&](size_t i) -> const Type * {
    if (i >= ref.args.size()) {
      return Type::voidTy();
    }
    const TypeRef &arg = ref.args[i];
    if (arg.kind == TypeRef::Kind::Name &&
        (arg.name == "void" || arg.name == "undefined" || arg.name == "unknown")) {
      return Type::voidTy();
    }

    return sema_.typeResolver().resolveType(arg, /*allowVoid=*/false);
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
    sema_.diags_.error(fn->returnType->loc,
                 "an async generator returns an async iterator: declare its type as "
                 "'AsyncGenerator<" +
                     (yielded->isVoid() ? std::string("int") : yielded->name().str()) + ">'");
    return Type::error();
  }
  if (declared->isIterator()) {
    return declared;
  }
  sema_.diags_.error(fn->returnType->loc,
               "a generator returns an iterator: declare its type as 'Generator<" +
                   (declared->isVoid() ? std::string("int") : declared->name().str()) + ">'");

  return Type::error();
}

const Type *GeneratorChecker::inferGeneratorType(const FuncSymbol *fn, llvm::ArrayRef<YieldExpr *> yields,
                                     llvm::ArrayRef<ReturnStmt *> returns, bool fallsOff,
                                     bool report) {
  const Type *yielded = nullptr;
  bool broken = false; // a yield whose error is reported already
  // An async generator awaits what it yields and returns.
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
        sema_.diags_.error(yield->loc(), quotedName(fn->name) + " yields '" + t->name() + "' here and '" +
                                       yielded->name() +
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
    sema_.diags_.error(fn->loc, "cannot tell what " + quotedName(fn->name) + " yields" + why +
                              "; declare its type, e.g. ': Generator<int>'");
    return Type::error();
  }

  // What it returns when it is done: void without a value.
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
        sema_.diags_.error(ret->value->loc(), quotedName(fn->name) + " returns '" + t->name() +
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
  // `xs: Iterable<int>` in generic code: what any copy iterates gives.
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
    return Type::string(); // its lines
  }
  const Type *element = iteratedType(type);
  if (!element) {
    return nullptr;
  }
  // A Promise<void> gives undefined.
  const Type *awaited = AsyncChecker::awaitedType(element);

  return awaited && awaited->isVoid() ? Type::undefinedTy() : awaited;
}

bool GeneratorChecker::checkSendsNothing(const Type *iterator, llvm::SMLoc loc, llvm::StringRef what) {
  const Type *sent = iterator->sent();
  if (sent->isVoid() || sent->isOptional() || sent->isError()) {
    return true;
  }
  sema_.diags_.error(loc, what + " resumes each 'yield' of '" + iterator->name() +
                        "' without a value, but it expects '" + sent->name() +
                        "' from next(x); call next(x) yourself, or let the generator take '" +
                        sent->name() + " | undefined'");

  return false;
}

Expr *GeneratorChecker::checkYield(YieldExpr *yield) {
  yield->type = Type::error();
  FuncSymbol *fn = sema_.currentFunction_;
  // The generator of safeTry gives no values: its `yield* r` takes the
  // value of a Result (see sema_result.cpp).
  if (fn && fn->decl && fn->decl->safeTry) {
    return sema_.safeTryChecker().checkSafeTryYield(yield);
  }
  if (!fn || !fn->decl || !fn->decl->isGenerator) {
    // The parser reports `yield` outside a generator.
    if (yield->value) {
      yield->value = sema_.exprChecker().checkExpr(yield->value);
    }
    return yield;
  }
  // The generator's type, if it is known: declared, or not being inferred.
  bool async = fn->decl->isAsync;
  const Type *generator =
      fn->ret && (async ? fn->ret->isAsyncIterator() : fn->ret->isIterator()) ? fn->ret : nullptr;
  const Type *yielded = generator ? generator->yielded() : nullptr;
  const Type *sent = generator ? generator->sent() : Type::voidTy();
  bool loose = fn->ret && !generator; // its type is wrong: no checks
  if (async && !sent->isVoid() && !loose) {
    sema_.diags_.error(yield->loc(), "values sent with next(x) to an async generator are not "
                               "supported yet; declare it as 'AsyncGenerator<" +
                                   (yielded ? yielded->name().str() : std::string("int")) + ">'");
    sent = Type::voidTy();
  }

  if (!yield->delegate && async) {
    // `yield p` of an async generator awaits p, and gives what it gives.
    if (yield->value) {
      yield->value = sema_.exprChecker().checkExpr(yield->value, yielded);
      const Type *t = yield->value->type;
      const Type *given = t->isError() ? t : AsyncChecker::awaitedType(t);
      if (!given) {
        sema_.diags_.error(yield->value->loc(), "'yield' of '" + t->name() +
                                              "' gives the value of the Promise or the value "
                                              "itself, which make no one type; check what it "
                                              "holds first");
      } else if (yielded && !given->isError() && given == t) {
        yield->value = sema_.typeRelations().coerce(yield->value, yielded);
      } else if (yielded && !given->isError() && !TypeRelations::isImplicitlyConvertible(given, yielded)) {
        sema_.diags_.error(yield->value->loc(), "the Promise gives '" + given->name() + "', but " +
                                              quotedName(fn->name) + " yields '" + yielded->name() +
                                              "'");
      }
    } else if (yielded && !yielded->isOptional() && !yielded->isError()) {
      sema_.diags_.error(yield->loc(), "'yield' without a value gives undefined, but " +
                                     quotedName(fn->name) + " yields '" + yielded->name() + "'");
    }
  } else if (yield->delegate && async) {
    // `yield* xs`: each value of an async iterator, or each element of xs
    // awaited.
    yield->value = sema_.dictChecker().iterableOf(sema_.exprChecker().checkExpr(yield->value));
    const Type *type = yield->value->type;
    const Type *element = asyncIteratedType(type);
    if (!element) {
      if (!type->isError()) {
        sema_.diags_.error(yield->value->loc(), "'yield*' in an async generator needs an async "
                                          "iterator, an array or an iterator, found '" +
                                              type->name() + "'");
      }
    } else if (yielded && !TypeRelations::isImplicitlyConvertible(element, yielded)) {
      sema_.diags_.error(yield->value->loc(), "'yield*' gives values of type '" + element->name() +
                                            "', but " + quotedName(fn->name) + " yields '" +
                                            yielded->name() + "'");
    }
    yield->element = element ? element : Type::error();
  } else if (!yield->delegate) {
    if (yield->value) {
      yield->value = sema_.exprChecker().checkExpr(yield->value, yielded);
      if (yielded) {
        yield->value = sema_.typeRelations().coerce(yield->value, yielded);
      }
    } else if (yielded && !yielded->isOptional() && !yielded->isError()) {
      sema_.diags_.error(yield->loc(), "'yield' without a value gives undefined, but " +
                                     quotedName(fn->name) + " yields '" + yielded->name() + "'");
    }
  } else {
    // `yield* xs`: each value of xs in turn. A chain of helpers runs there.
    sema_.iteratorMethods().markFused(yield->value);
    yield->value = sema_.dictChecker().iterableOf(sema_.exprChecker().checkExpr(yield->value));
    const Type *type = yield->value->type;
    const Type *element = iteratedType(type);
    if (!element) {
      if (type->isResult()) {
        sema_.diags_.error(yield->value->loc(),
                     "'yield*' of a Result takes its value only in the generator given to "
                     "safeTry, 'safeTry(function* () { ... })'; this generator gives values, "
                     "and 'yield*' needs an array, a string or an iterator here");
      } else if (!type->isError()) {
        sema_.diags_.error(yield->value->loc(),
                     "'yield*' needs an array, a string or an iterator, found '" + type->name() +
                         "'");
      }
    } else if (type->isIterator() && !type->sent()->isVoid() && type->sent() != sent && !loose) {
      // What next() sends to this generator goes on to the other one.
      sema_.diags_.error(yield->value->loc(),
                   "'yield*' passes on what next(x) sends, and '" + type->name() + "' expects '" +
                       type->sent()->name() + "' from it, but " + quotedName(fn->name) + " gets " +
                       (sent->isVoid() ? "nothing" : "'" + sent->name().str() + "'"));
    } else if (yielded && !TypeRelations::isImplicitlyConvertible(element, yielded)) {
      sema_.diags_.error(yield->value->loc(), "'yield*' gives values of type '" + element->name() +
                                            "', but " + quotedName(fn->name) + " yields '" +
                                            yielded->name() + "'");
    }
    yield->element = element ? element : Type::error();
  }
  if (!fn->ret && sema_.yields_) {
    sema_.yields_->push_back(yield);
  }
  // Whoever iterates the generator may change anything before it goes on.
  sema_.flowChecker().calledFunction();

  if (yield->delegate) {
    // What the other generator returns, if anything; not kept of an async
    // one yet.
    const Type *type = yield->value->type;
    yield->type = !async && type->isIterator() && !type->returned()->isVoid() ? type->returned()
                                                                              : Type::undefinedTy();
  } else if (async) {
    yield->type = Type::undefinedTy();
  } else if (!sent->isVoid()) {
    yield->type = sent;
  } else if (yield->discarded || loose) {
    yield->type = Type::undefinedTy();
  } else {
    sema_.diags_.error(yield->loc(),
                 "the value of 'yield' is what next(x) sends, and nothing says its type; "
                 "declare it in the return type, e.g. ': Generator<" +
                     (yielded ? yielded->name().str() : std::string("int")) + ", void, string>'");
  }

  return yield;
}

Expr *GeneratorChecker::destructuredIterator(Expr *init, const VarDeclarator &d) {
  const Type *iterator = init->type;
  checkSendsNothing(iterator, init->loc(), "destructuring");
  Expr *source = init;
  if (!d.patternRest) {
    // As many values as the pattern names; the iterator is left after them.
    auto *member = sema_.ctx_.make<MemberExpr>(init->loc(), init, "take");
    auto *n = sema_.ctx_.make<IntLitExpr>(init->loc(), d.patternLength);
    n->type = Type::longTy();
    auto *take = sema_.ctx_.make<CallExpr>(init->loc(), member, std::vector<Expr *>{n});
    take->builtin = Builtin::IterTake;
    take->fused = true;
    take->type = Type::iterator(iterator->yielded());
    source = take;
  }
  auto *array = sema_.ctx_.make<ArrayLitExpr>(init->loc());
  array->elements.push_back({source, /*spread=*/true});
  array->type = Type::array(iterator->yielded());
  sema_.flowChecker().calledFunction(); // a generator runs

  return array;
}

} // namespace tz
