// Generators and iterators (docs/iterators.md): the types `Generator<T, R,
// N>` and `IterableIterator<T>`, `yield` and `yield*`, next(), the helpers
// of ES2025 (map, filter, take, drop, flatMap, reduce, toArray, forEach,
// some, every, find), Iterator.from, Array.from and the iterators of arrays.
//
// A generator returns an iterator; the types of what it yields and returns
// come from its body unless they are declared, as a return type does. What
// next(x) sends to a `yield` must be declared, as TypeScript requires.

#include "compiler/check/sema/sema.h"

#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// The helpers that make a stage of a lazy chain.
const llvm::StringLiteral kStages[] = {"map", "filter", "take", "drop", "flatMap"};

} // namespace

const Type *Sema::resolveIteratorType(const TypeRef &ref) {
  llvm::StringRef name = ref.name;
  // `IteratorResult<T, R>`, what next() of an iterator written by hand
  // gives.
  if (name == "IteratorResult" && !lookupType(name)) {
    if (ref.args.empty() || ref.args.size() > 2) {
      diags_.error(ref.loc, "'IteratorResult' takes 1-2 type arguments: the values and what the "
                            "iterator gives when it is done, e.g. 'IteratorResult<int, undefined>'");
      return Type::error();
    }
    const Type *value = resolveType(ref.args[0], /*allowVoid=*/false);
    const Type *returned = Type::undefinedTy();
    if (ref.args.size() == 2) {
      const TypeRef &arg = ref.args[1];
      if (!(arg.kind == TypeRef::Kind::Name &&
            (arg.name == "void" || arg.name == "undefined" || arg.name == "unknown")))
        returned = resolveType(arg, /*allowVoid=*/false);
    }
    if (value->isError() || returned->isError())
      return Type::error();
    return iteratorResultType(value, returned);
  }
  // `Iterable<T>` makes a function generic, as the type of a parameter (see
  // iterableParam); anywhere else it is no one type.
  if (name == "Iterable" && !lookupType(name)) {
    diags_.error(ref.loc, "'Iterable<T>' can only be the type of a parameter of a function declared "
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
      !(name == "Iterator" && !lookupType(name)) && !async)
    return nullptr;
  if (ref.args.empty() || ref.args.size() > 3) {
    diags_.error(ref.loc, "'" + name +
                              "' takes 1-3 type arguments: the values it gives, what it "
                              "returns and what next() sends, e.g. '" +
                              name + "<int>'");
    return Type::error();
  }
  const Type *yielded = resolveType(ref.args[0], /*allowVoid=*/false);
  // What it returns and what next() sends: void for nothing, which
  // undefined and TypeScript's unknown also say.
  auto other = [&](size_t i) -> const Type * {
    if (i >= ref.args.size())
      return Type::voidTy();
    const TypeRef &arg = ref.args[i];
    if (arg.kind == TypeRef::Kind::Name &&
        (arg.name == "void" || arg.name == "undefined" || arg.name == "unknown"))
      return Type::voidTy();
    return resolveType(arg, /*allowVoid=*/false);
  };
  const Type *returned = other(1), *sent = other(2);
  for (const Type *t : {yielded, returned, sent})
    if (t->isError())
      return t;
  if (async)
    return Type::asyncIterator(yielded, returned, sent);
  return Type::iterator(yielded, returned, sent);
}

const Type *Sema::generatorReturnType(const FunctionDecl *fn, const Type *declared) {
  if (!fn->isGenerator || declared->isError())
    return declared;
  if (fn->isAsync) {
    if (declared->isAsyncIterator())
      return declared;
    const Type *yielded = declared->isIterator() ? declared->yielded() : declared;
    diags_.error(fn->returnType->loc,
                 "an async generator returns an async iterator: declare its type as "
                 "'AsyncGenerator<" +
                     (yielded->isVoid() ? std::string("int") : yielded->name().str()) + ">'");
    return Type::error();
  }
  if (declared->isIterator())
    return declared;
  diags_.error(fn->returnType->loc,
               "a generator returns an iterator: declare its type as 'Generator<" +
                   (declared->isVoid() ? std::string("int") : declared->name().str()) + ">'");
  return Type::error();
}

const Type *Sema::inferGeneratorType(const FuncSymbol *fn, llvm::ArrayRef<YieldExpr *> yields,
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
    if (async && !yield->delegate && t && !t->isError())
      t = awaitedType(t);
    if (!t || t->isError()) {
      broken = true;
      continue;
    }
    const Type *common = yielded ? unionUnify(yielded, t) : t;
    if (!common || common->isError()) {
      if (report)
        diags_.error(yield->loc(), quotedName(fn->name) + " yields '" + t->name() + "' here and '" +
                                       yielded->name() +
                                       "' elsewhere; a generator yields values of "
                                       "one type");
      return Type::error();
    }
    yielded = common;
  }
  if (!yielded || yielded->isUndefined()) {
    if (!report)
      return nullptr;
    if (broken && !yielded)
      return Type::error();
    std::string why = yields.empty() ? ": it has no 'yield'"
                      : yielded      ? ": it yields only undefined"
                                     : "";
    diags_.error(fn->loc, "cannot tell what " + quotedName(fn->name) + " yields" + why +
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
    if (t->isError())
      continue;
    if (async)
      t = awaitedType(t);
    if (!t || t->isUndefined()) {
      undefined = true;
      continue;
    }
    const Type *common = returned ? unionUnify(returned, t) : t;
    if (!common || common->isError()) {
      if (report)
        diags_.error(ret->value->loc(), quotedName(fn->name) + " returns '" + t->name() +
                                            "' here and '" + returned->name() +
                                            "' elsewhere; a function returns values "
                                            "of one type");
      return Type::error();
    }
    returned = common;
  }
  if (!returned)
    returned = Type::voidTy();
  else if (undefined && !returned->isVoid())
    returned = Type::optional(returned);
  if (async)
    return Type::asyncIterator(yielded, returned, Type::voidTy());
  return Type::iterator(yielded, returned, Type::voidTy());
}

const Type *Sema::iteratedType(const Type *type) {
  if (type->isArray())
    return type->inner();
  if (type->isString())
    return type;
  if (type->isIterator())
    return type->yielded();
  if (type->isTypedArray())
    return type->elementType();
  // `xs: Iterable<int>` in generic code: what any copy iterates gives.
  if (type->isParam() && type->iterated())
    return type->iterated();
  return nullptr;
}

const Type *Sema::asyncIteratedType(const Type *type) {
  if (type->isAsyncIterator())
    return type->yielded();
  if (type->runtimeKind() == Type::Runtime::Readline ||
      type->runtimeKind() == Type::Runtime::ReadlinePromises)
    return Type::string(); // its lines
  const Type *element = iteratedType(type);
  if (!element)
    return nullptr;
  // A Promise<void> gives undefined.
  const Type *awaited = awaitedType(element);
  return awaited && awaited->isVoid() ? Type::undefinedTy() : awaited;
}

bool Sema::checkSendsNothing(const Type *iterator, llvm::SMLoc loc, llvm::StringRef what) {
  const Type *sent = iterator->sent();
  if (sent->isVoid() || sent->isOptional() || sent->isError())
    return true;
  diags_.error(loc, what + " resumes each 'yield' of '" + iterator->name() +
                        "' without a value, but it expects '" + sent->name() +
                        "' from next(x); call next(x) yourself, or let the generator take '" +
                        sent->name() + " | undefined'");
  return false;
}

void Sema::markFused(Expr *expr) const {
  while (auto *call = dyn_cast<CallExpr>(expr)) {
    auto *member = dyn_cast<MemberExpr>(call->callee);
    if (!member || member->optional || call->optional)
      return;
    // `Iterator.from(x)` of an iterator is the iterator itself.
    if (isGlobalName(member->object, "Iterator") && member->name == "from" &&
        call->args.size() == 1) {
      expr = call->args[0];
      continue;
    }
    if (!llvm::is_contained(kStages, member->name))
      return;
    call->fused = true;
    expr = member->object;
  }
}

Expr *Sema::checkYield(YieldExpr *yield) {
  yield->type = Type::error();
  FuncSymbol *fn = currentFunction_;
  // The generator of safeTry gives no values: its `yield* r` takes the
  // value of a Result (see sema_result.cpp).
  if (fn && fn->decl && fn->decl->safeTry)
    return checkSafeTryYield(yield);
  if (!fn || !fn->decl || !fn->decl->isGenerator) {
    // The parser reports `yield` outside a generator.
    if (yield->value)
      yield->value = checkExpr(yield->value);
    return yield;
  }
  // The generator's type, if it is known: declared, or not being inferred.
  bool async = fn->decl->isAsync;
  const Type *generator = fn->ret && (async ? fn->ret->isAsyncIterator() : fn->ret->isIterator())
                              ? fn->ret
                              : nullptr;
  const Type *yielded = generator ? generator->yielded() : nullptr;
  const Type *sent = generator ? generator->sent() : Type::voidTy();
  bool loose = fn->ret && !generator; // its type is wrong: no checks
  if (async && !sent->isVoid() && !loose) {
    diags_.error(yield->loc(), "values sent with next(x) to an async generator are not "
                               "supported yet; declare it as 'AsyncGenerator<" +
                                   (yielded ? yielded->name().str() : std::string("int")) + ">'");
    sent = Type::voidTy();
  }

  if (!yield->delegate && async) {
    // `yield p` of an async generator awaits p, and gives what it gives.
    if (yield->value) {
      yield->value = checkExpr(yield->value, yielded);
      const Type *t = yield->value->type;
      const Type *given = t->isError() ? t : awaitedType(t);
      if (!given) {
        diags_.error(yield->value->loc(), "'yield' of '" + t->name() +
                                              "' gives the value of the Promise or the value "
                                              "itself, which make no one type; check what it "
                                              "holds first");
      } else if (yielded && !given->isError() && given == t) {
        yield->value = coerce(yield->value, yielded);
      } else if (yielded && !given->isError() && !isImplicitlyConvertible(given, yielded)) {
        diags_.error(yield->value->loc(), "the Promise gives '" + given->name() + "', but " +
                                              quotedName(fn->name) + " yields '" +
                                              yielded->name() + "'");
      }
    } else if (yielded && !yielded->isOptional() && !yielded->isError()) {
      diags_.error(yield->loc(), "'yield' without a value gives undefined, but " +
                                     quotedName(fn->name) + " yields '" + yielded->name() + "'");
    }
  } else if (yield->delegate && async) {
    // `yield* xs`: each value of an async iterator, or each element of xs
    // awaited.
    yield->value = iterableOf(checkExpr(yield->value));
    const Type *type = yield->value->type;
    const Type *element = asyncIteratedType(type);
    if (!element) {
      if (!type->isError())
        diags_.error(yield->value->loc(), "'yield*' in an async generator needs an async "
                                          "iterator, an array or an iterator, found '" +
                                              type->name() + "'");
    } else if (yielded && !isImplicitlyConvertible(element, yielded)) {
      diags_.error(yield->value->loc(), "'yield*' gives values of type '" + element->name() +
                                            "', but " + quotedName(fn->name) + " yields '" +
                                            yielded->name() + "'");
    }
    yield->element = element ? element : Type::error();
  } else if (!yield->delegate) {
    if (yield->value) {
      yield->value = checkExpr(yield->value, yielded);
      if (yielded)
        yield->value = coerce(yield->value, yielded);
    } else if (yielded && !yielded->isOptional() && !yielded->isError()) {
      diags_.error(yield->loc(), "'yield' without a value gives undefined, but " +
                                     quotedName(fn->name) + " yields '" + yielded->name() + "'");
    }
  } else {
    // `yield* xs`: each value of xs in turn. A chain of helpers runs there.
    markFused(yield->value);
    yield->value = iterableOf(checkExpr(yield->value));
    const Type *type = yield->value->type;
    const Type *element = iteratedType(type);
    if (!element) {
      if (type->isResult())
        diags_.error(yield->value->loc(),
                     "'yield*' of a Result takes its value only in the generator given to "
                     "safeTry, 'safeTry(function* () { ... })'; this generator gives values, "
                     "and 'yield*' needs an array, a string or an iterator here");
      else if (!type->isError())
        diags_.error(yield->value->loc(),
                     "'yield*' needs an array, a string or an iterator, found '" + type->name() +
                         "'");
    } else if (type->isIterator() && !type->sent()->isVoid() && type->sent() != sent && !loose) {
      // What next() sends to this generator goes on to the other one.
      diags_.error(yield->value->loc(),
                   "'yield*' passes on what next(x) sends, and '" + type->name() + "' expects '" +
                       type->sent()->name() + "' from it, but " + quotedName(fn->name) + " gets " +
                       (sent->isVoid() ? "nothing" : "'" + sent->name().str() + "'"));
    } else if (yielded && !isImplicitlyConvertible(element, yielded)) {
      diags_.error(yield->value->loc(), "'yield*' gives values of type '" + element->name() +
                                            "', but " + quotedName(fn->name) + " yields '" +
                                            yielded->name() + "'");
    }
    yield->element = element ? element : Type::error();
  }
  if (!fn->ret && yields_)
    yields_->push_back(yield);
  // Whoever iterates the generator may change anything before it goes on.
  calledFunction();

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
    diags_.error(yield->loc(),
                 "the value of 'yield' is what next(x) sends, and nothing says its type; "
                 "declare it in the return type, e.g. ': Generator<" +
                     (yielded ? yielded->name().str() : std::string("int")) + ", void, string>'");
  }
  return yield;
}

Expr *Sema::checkArrayIterator(CallExpr *call, MemberExpr *member) {
  const Type *element = member->object->type->inner();
  llvm::StringRef name = member->name;
  if (!checkArity(call, member->loc(), 0, 0))
    return call;
  if (name == "keys") {
    call->builtin = Builtin::ArrKeys;
    call->type = Type::iterator(Type::intTy());
  } else if (name == "values") {
    call->builtin = Builtin::ArrValues;
    call->type = Type::iterator(element);
  } else {
    call->builtin = Builtin::ArrEntries;
    call->type = Type::iterator(Type::tuple({Type::intTy(), element}));
  }
  return call;
}

Expr *Sema::checkIteratorFrom(CallExpr *call, MemberExpr *member) {
  if (rejectSpread(call))
    return call;
  if (member->name != "from") {
    diags_.error(member->loc(), "'Iterator." + member->name +
                                    "' is not supported; of Iterator only 'Iterator.from' is");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  if (!checkArity(call, member->loc(), 1, 1))
    return call;
  call->args[0] = iterableOf(checkExpr(call->args[0]));
  const Type *type = call->args[0]->type;
  const Type *element = iteratedType(type);
  if (!element) {
    if (!type->isError())
      diags_.error(call->args[0]->loc(),
                   "'Iterator.from' needs an array, a string or an iterator, found '" +
                       type->name() + "'");
    return call;
  }
  call->builtin = Builtin::IterFrom;
  // An iterator is its own: Iterator.from gives it back.
  call->type = type->isIterator() ? type : Type::iterator(element);
  return call;
}

Expr *Sema::checkArrayFrom(CallExpr *call, MemberExpr *member) {
  if (rejectSpread(call))
    return call;
  if (member->name != "from") {
    diags_.error(member->loc(),
                 "'Array." + member->name +
                     "' is not supported; of Array only 'Array.from' and 'new Array' are");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  if (!checkArity(call, member->loc(), 1, 2))
    return call;
  markFused(call->args[0]);
  call->args[0] = iterableOf(checkExpr(call->args[0]));
  const Type *type = call->args[0]->type;
  const Type *element = iteratedType(type);
  if (!element) {
    if (!type->isError())
      diags_.error(call->args[0]->loc(),
                   "'Array.from' needs an array, a string or an iterator, found '" + type->name() +
                       "'");
    if (call->args.size() == 2)
      call->args[1] = checkExpr(call->args[1]);
    return call;
  }
  if (type->isIterator())
    checkSendsNothing(type, call->args[0]->loc(), "'Array.from'");
  call->builtin = Builtin::ArrayFrom;
  call->type = Type::array(element);
  if (call->args.size() == 2) {
    // `Array.from(xs, (x, i) => ...)`: each value through the function.
    Signature sig;
    sig.pad = false;
    sig.params = {element, Type::intTy()};
    call->args[1] = checkFunctionArg(call->args[1], sig, 1, "Array.from");
    const Type *callback = call->args[1]->type;
    if (!callback->isFunction())
      return call;
    const Type *result = callback->ret();
    if (result->isVoid() || result->isUndefined()) {
      diags_.error(call->args[1]->loc(), "the callback of 'Array.from' must return a value");
      call->type = Type::error();
      return call;
    }
    call->type = Type::array(result);
  }
  calledFunction(); // a generator or the callback may change anything
  return call;
}

Expr *Sema::checkIteratorMethod(CallExpr *call, MemberExpr *member) {
  const Type *iterator = member->object->type;
  const Type *element = iterator->yielded();
  llvm::StringRef name = member->name;
  size_t count = call->args.size();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };

  if (name == "next") {
    if (!checkArity(call, member->loc(), 0, 1))
      return call;
    // The value is undefined when the iterator is done, or what a generator
    // returns then: `T | R | undefined`.
    const Type *returned = iterator->returned();
    const Type *value = returned->isVoid() ? element : Type::unionOf({element, returned});
    if (std::string problem = unionProblem(value); !problem.empty()) {
      diags_.error(member->loc(),
                   "next() of '" + iterator->name() + "' gives '" + element->name() + "', or '" +
                       returned->name() +
                       "' when it is done, and no union holds both: " + problem +
                       "; iterate it with 'for...of', which ignores what it returns");
      checkArgsLoosely();
      return call;
    }
    if (count == 1) {
      // What a generator that ignores it gets is evaluated and dropped.
      const Type *sent = iterator->sent();
      call->args[0] = sent->isVoid() ? checkExpr(call->args[0]) : checkArg(call->args[0], sent);
    }
    call->builtin = Builtin::IterNext;
    call->type =
        Type::object({{"value", Type::optional(value), false}, {"done", Type::boolTy(), false}},
                     "IteratorResult<" + value->name().str() + ">");
    calledFunction(); // the generator runs
    return call;
  }
  if (name == "return" || name == "throw") {
    diags_.error(member->loc(), "'" + name +
                                    "()' of an iterator is not supported yet: it comes with "
                                    "exceptions");
    checkArgsLoosely();
    return call;
  }

  struct Helper {
    llvm::StringLiteral name;
    Builtin builtin;
  };
  static const Helper helpers[] = {
      {"map", Builtin::IterMap},         {"filter", Builtin::IterFilter},
      {"take", Builtin::IterTake},       {"drop", Builtin::IterDrop},
      {"flatMap", Builtin::IterFlatMap}, {"toArray", Builtin::IterToArray},
      {"forEach", Builtin::IterForEach}, {"reduce", Builtin::IterReduce},
      {"some", Builtin::IterSome},       {"every", Builtin::IterEvery},
      {"find", Builtin::IterFind},
  };
  const Helper *helper = nullptr;
  for (const Helper &h : helpers)
    if (h.name == name)
      helper = &h;
  if (!helper) {
    std::string message =
        "property '" + name.str() + "' does not exist on type '" + iterator->name().str() + "'";
    if (name == "length")
      message +=
          "; an iterator does not know how many values it gives: collect them with '[...it]'";
    diags_.error(member->loc(), message);
    checkArgsLoosely();
    return call;
  }
  Builtin builtin = helper->builtin;
  // The helpers call next() without a value.
  if (!checkSendsNothing(iterator, member->loc(), "'" + name.str() + "'")) {
    checkArgsLoosely();
    return call;
  }

  if (builtin == Builtin::IterTake || builtin == Builtin::IterDrop) {
    if (!checkArity(call, member->loc(), 1, 1))
      return call;
    call->args[0] = checkPosition(call->args[0]);
    call->builtin = builtin;
    call->type = Type::iterator(element);
    return call;
  }
  if (builtin == Builtin::IterToArray) {
    if (!checkArity(call, member->loc(), 0, 0))
      return call;
    call->builtin = builtin;
    call->type = Type::array(element);
    calledFunction(); // the iterator runs
    return call;
  }

  bool reduce = builtin == Builtin::IterReduce;
  if (count == 0) {
    diags_.error(member->loc(), "'" + name + "' needs a function");
    return call;
  }
  if (count > (reduce ? 2u : 1u)) {
    diags_.error(member->loc(), "expected " + argumentCount(1, reduce ? 2 : 1) + ", but got " +
                                    llvm::Twine(count));
    checkArgsLoosely();
    return call;
  }
  // The callback gets the value and a counter (and first the accumulator
  // for reduce), not an index and an array as for arrays.
  Signature sig;
  sig.pad = false;
  unsigned primary = 1;
  const Type *accumulator = element;
  switch (builtin) {
  case Builtin::IterForEach: sig.voidResult = true; break;
  case Builtin::IterFilter:
  case Builtin::IterSome:
  case Builtin::IterEvery:
  case Builtin::IterFind: sig.ret = Type::boolTy(); break;
  case Builtin::IterReduce:
    primary = 2;
    if (count == 2) {
      Expr *&init = call->args[1];
      auto *unary = dyn_cast<UnaryExpr>(init);
      bool literal = isa<IntLitExpr>(init) ||
                     (unary && unary->op == UnaryOp::Neg && isa<IntLitExpr>(unary->operand));
      init = checkExpr(init, literal && apparentType(element)->isNumeric() ? apparentType(element)
                                                                           : nullptr);
      accumulator = init->type;
      if (accumulator->isUndefined() || accumulator->isVoid()) {
        diags_.error(init->loc(),
                     "the initial value of 'reduce' needs a type; declare it in a variable "
                     "first, e.g. 'const start: int | undefined = undefined'");
        accumulator = Type::error();
      }
    }
    sig.ret = accumulator;
    break;
  default: break;
  }
  if (accumulator->isError()) {
    call->args[0] = checkExpr(call->args[0]);
    return call;
  }
  sig.params = reduce ? std::vector<const Type *>{accumulator, element, Type::intTy()}
                      : std::vector<const Type *>{element, Type::intTy()};
  bool isBoolean = isGlobalName(call->args[0], "Boolean");
  call->args[0] = checkFunctionArg(call->args[0], sig, primary, name);
  // A stage that is not consumed where it is written keeps its callback.
  if (isIteratorStage(builtin))
    if (auto *fe = dyn_cast<FunctionExpr>(call->args[0]))
      fe->fn->nonEscaping = call->fused;
  call->builtin = builtin;
  if (isIteratorSink(builtin))
    calledFunction(); // the callback may change anything
  const Type *callback = call->args[0]->type;
  if (!callback->isFunction())
    return call;
  const Type *result = callback->ret();
  switch (builtin) {
  case Builtin::IterMap:
    if (result->isVoid() || result->isUndefined()) {
      diags_.error(call->args[0]->loc(), "the callback of 'map' must return a value");
      return call;
    }
    call->type = Type::iterator(result);
    break;
  case Builtin::IterFlatMap: {
    // Each result is iterated in turn. JavaScript rejects a string here.
    const Type *inner = result->isString() ? nullptr : iteratedType(result);
    if (!inner) {
      diags_.error(call->args[0]->loc(),
                   "the callback of 'flatMap' must return an array or an iterator, "
                   "found '" +
                       result->name() + "'");
      return call;
    }
    if (result->isIterator() && !checkSendsNothing(result, call->args[0]->loc(), "'flatMap'"))
      return call;
    call->type = Type::iterator(inner);
    break;
  }
  case Builtin::IterFilter:
    // `filter(x => x != undefined)` keeps only the values that hold one.
    if (const Type *kept = filteredType(call->args[0], element))
      call->type = Type::iterator(kept);
    else
      call->type = Type::iterator(
          element->isOptional() && (isBoolean || keepsDefined(call->args[0])) ? element->inner()
                                                                              : element);
    break;
  case Builtin::IterForEach: call->type = Type::voidTy(); break;
  case Builtin::IterReduce:
    call->type = accumulator;
    if (count == 2 && call->args.size() == 2)
      call->args[1] = coerce(call->args[1], accumulator);
    break;
  case Builtin::IterSome:
  case Builtin::IterEvery: call->type = Type::boolTy(); break;
  case Builtin::IterFind: call->type = Type::optional(element); break;
  default: break;
  }
  return call;
}

Expr *Sema::destructuredIterator(Expr *init, const VarDeclarator &d) {
  const Type *iterator = init->type;
  checkSendsNothing(iterator, init->loc(), "destructuring");
  Expr *source = init;
  if (!d.patternRest) {
    // As many values as the pattern names; the iterator is left after them.
    auto *member = ctx_.make<MemberExpr>(init->loc(), init, "take");
    auto *n = ctx_.make<IntLitExpr>(init->loc(), d.patternLength);
    n->type = Type::longTy();
    auto *take = ctx_.make<CallExpr>(init->loc(), member, std::vector<Expr *>{n});
    take->builtin = Builtin::IterTake;
    take->fused = true;
    take->type = Type::iterator(iterator->yielded());
    source = take;
  }
  auto *array = ctx_.make<ArrayLitExpr>(init->loc());
  array->elements.push_back({source, /*spread=*/true});
  array->type = Type::array(iterator->yielded());
  calledFunction(); // a generator runs
  return array;
}

} // namespace tz
