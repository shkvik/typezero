// Async (docs/async.md, step 1): async functions and `await`, the type
// Promise<T> with its methods and the functions of Promise, timers,
// queueMicrotask and process.nextTick, and the Promises nothing waits for.
//
// An async function returns a Promise of what its `return` statements give;
// `return p` of a Promise gives what p gives, as in JavaScript. `await` takes
// the value of a Promise and gives back any other value; it is allowed in an
// async function and in top-level code. A Promise that a statement drops is
// an error, as a dropped Result is: nothing would wait for it, and an error
// in it would end the program later. `void p` drops one on purpose.

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

const Type *literalType(const Expr *value);

namespace {

// What `await` of a value of `type` gives: the value of a Promise, or the
// value itself.
const Type *awaited(const Type *type) { return type->isPromise() ? type->promised() : type; }

// The text of a statement that starts at `at`, for hints: up to the end of
// its line, without a `;` or a comment at the end.
std::string statementText(llvm::SMLoc at) {
  if (!at.isValid())
    return "";
  const char *start = at.getPointer(), *end = start;
  int depth = 0;
  for (; *end && *end != '\n' && *end != '\r'; ++end) {
    if (*end == '(' || *end == '[' || *end == '{')
      ++depth;
    else if (*end == ')' || *end == ']' || *end == '}')
      --depth;
    else if (depth == 0 && (*end == ';' || (end[0] == '/' && end[1] == '/')))
      break;
  }
  llvm::StringRef text = llvm::StringRef(start, size_t(end - start)).rtrim();
  return depth == 0 && text.size() <= 60 ? text.str() : "";
}

} // namespace

const Type *Sema::resolvePromiseType(const TypeRef &ref) {
  if (ref.name != "Promise" && ref.name != "PromiseLike")
    return nullptr;
  if (ref.args.size() != 1) {
    diags_.error(ref.loc, "'" + ref.name +
                              "' takes one type argument, the type of its value, e.g. '" +
                              ref.name + "<int>'");
    return Type::error();
  }
  const Type *value = resolveType(ref.args[0], /*allowVoid=*/true);
  // A Promise of a Promise is a Promise of its value in JavaScript.
  return Type::promise(awaited(value));
}

const Type *Sema::asyncReturnType(const FunctionDecl *fn, const Type *declared) {
  if (!fn->isAsync || fn->isGenerator || declared->isError() || declared->isPromise())
    return declared;
  diags_.error(fn->returnType->loc, "an async function returns a Promise: declare its type as "
                                    "'Promise<" +
                                        declared->name().str() + ">'");
  return Type::promise(declared);
}

const Type *Sema::inferAsyncType(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                                 bool fallsOff, bool report) {
  const Type *type = nullptr;
  bool undefined = fallsOff, unknown = false;
  for (ReturnStmt *ret : returns) {
    if (!ret->value) {
      undefined = true;
      continue;
    }
    const Type *t = awaited(ret->value->type);
    if (t->isError()) {
      unknown = true;
      continue;
    }
    if (t->isUndefined() || t->isVoid()) {
      undefined = true;
      continue;
    }
    const Type *common = type ? unify(type, t) : t;
    if (!common || common->isError()) {
      if (report)
        diags_.error(ret->value->loc(), quotedName(fn->name) + " returns '" + t->name() +
                                            "' here and '" + type->name() +
                                            "' elsewhere; a function returns values of one type");
      return Type::error();
    }
    type = common;
  }
  if (!type) {
    if (unknown)
      return report ? Type::error() : nullptr;
    return Type::promise(Type::voidTy());
  }
  if (undefined)
    type = Type::optional(type);
  return Type::promise(type);
}

void Sema::finishAsyncReturns(FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns) {
  const Type *value = fn->ret->isPromise() ? fn->ret->promised() : nullptr;
  if (!value || value->isError())
    return;
  for (ReturnStmt *ret : returns) {
    if (!ret->value || ret->value->type->isError())
      continue;
    const Type *t = ret->value->type;
    if (t->isPromise()) {
      // `return p`: the Promise of the function follows p.
      if (t->promised() != value && !t->promised()->isNever())
        diags_.error(ret->value->loc(), "this Promise gives '" + t->promised()->name() + "', but " +
                                            quotedName(fn->name) + " gives '" + value->name() +
                                            "'; await it and convert the value");
      continue;
    }
    if (!value->isVoid())
      ret->value = convert(ret->value, value);
  }
}

//===----------------------------------------------------------------------===//
// await
//===----------------------------------------------------------------------===//

Expr *Sema::checkAwait(AwaitExpr *await, const Type *expected) {
  await->type = Type::error();
  file_->usesAsync = true;
  FuncSymbol *fn = currentFunction_;
  if (fn && !(fn->decl && fn->decl->isAsync)) {
    FunctionDecl *decl = fn->decl;
    std::string hint = decl && decl->isArrow         ? "'async (...) => ...'"
                       : decl && decl->cls           ? "'async " + decl->name.str() + "() { ... }'"
                       : decl && !decl->name.empty() ? "'async function " + decl->name.str() + "'"
                                                     : "'async function'";
    diags_.error(await->loc(), "'await' can only be used in an async function or in top-level "
                               "code; make the function async: " +
                                   hint);
  } else if (!fn) {
    file_->topLevelAwait = true;
  }
  // `await new Promise(...)` where an int is expected waits for a
  // Promise<int>.
  const Type *hint = nullptr;
  if (expected && !expected->isError() && !expected->isVoid() &&
      (isa<NewExpr>(await->value) || isa<CallExpr>(await->value)))
    hint = Type::promise(expected);
  await->value = checkExpr(await->value, hint);
  const Type *type = await->value->type;
  // Other tasks run while this one waits: they may change anything.
  calledFunction();
  if (type->isError())
    return await;
  const Type *result = awaitedType(type);
  if (!result) {
    diags_.error(await->value->loc(),
                 "'await' of '" + type->name() +
                     "' gives the value of the Promise or the value itself, which make no one "
                     "type; check what it holds first");
    return await;
  }
  await->type = result;
  return await;
}

const Type *Sema::awaitedType(const Type *type) {
  if (type->isPromise())
    return type->promised();
  if (type->isOptional()) {
    const Type *inner = awaitedType(type->inner());
    if (!inner || inner->isVoid())
      return inner;
    return Type::optional(inner);
  }
  if (!type->isUnion() ||
      llvm::none_of(type->members(), [](const Type *m) { return m->isPromise(); }))
    return type;
  const Type *result = nullptr;
  for (const Type *m : type->members()) {
    const Type *t = m->isPromise() ? m->promised() : m;
    result = result ? unionUnify(result, t) : t;
    if (!result || result->isError())
      return nullptr;
  }
  return result;
}

// `for await (const x of xs)` (docs/iterators.md, step 4): over the values
// of an async iterator, the lines of readline, or the elements of an array
// or an iterator, each awaited. It waits, as `await` does, so it goes where
// `await` goes.
void Sema::checkForAwait(ForOfStmt *stmt) {
  file_->usesAsync = true;
  FuncSymbol *fn = currentFunction_;
  if (fn && !(fn->decl && fn->decl->isAsync))
    diags_.error(stmt->loc(), "'for await' can only be used in an async function or in top-level "
                              "code; make the function async");
  else if (!fn)
    file_->topLevelAwait = true;
  markFused(stmt->iterable);
  stmt->iterable = apparent(checkExpr(stmt->iterable));
  // An object with `[Symbol.asyncIterator]()` gives what that gives; one
  // with `[Symbol.iterator]()`, its iterator.
  if (Expr *iterator = asyncUserIterator(stmt->iterable))
    stmt->iterable = iterator;
  else
    stmt->iterable = iterableOf(stmt->iterable);
  if (stmt->iterable->type->isMatch()) // the captures
    stmt->iterable = matchItems(stmt->iterable);
  const Type *type = stmt->iterable->type;
  const Type *element = asyncIteratedType(type);
  if (!element) {
    if (type->isOptional() && asyncIteratedType(type->inner()))
      diags_.error(stmt->iterable->loc(), "'" + type->name() +
                                              "' may be undefined; check it before iterating");
    else if (type->isRecord())
      diags_.error(stmt->iterable->loc(),
                   "a Record is not iterable, as an object is not in JavaScript; walk its keys "
                   "with 'for...in', or its entries with 'for (const [k, v] of "
                   "Object.entries(r))'");
    else if (!type->isError())
      diags_.error(stmt->iterable->loc(), "'for await' needs an async iterator, an array or an "
                                          "iterator, found '" +
                                              type->name() + "'");
    element = Type::error();
  } else if (type->isIterator()) {
    checkSendsNothing(type, stmt->iterable->loc(), "'for await'");
  }
  if (stmt->binding.typeAnn)
    diags_.error(stmt->binding.typeAnn->loc,
                 "a 'for await' variable cannot have a type annotation");
  if (stmt->binding.destructured && !stmt->binding.objectPattern && !element->isArray() &&
      !element->isTuple() && !element->isMatch() && !element->isError()) {
    diags_.error(stmt->binding.loc, "only arrays can be destructured; the elements are '" +
                                        element->name() + "'");
    element = Type::error();
  }
  // Other tasks run while it waits: they may change anything, as the
  // iterator itself may.
  calledFunction();
  LoopScan scan = scanLoop({stmt->body}, {});
  scan.callsFunctions = true;
  stmt->borrowElements = false;
  Flow head = loopHead(scan, nullptr, /*induction=*/false);
  flow_ = head;
  pushScope();
  stmt->binding.sym = declareVar(stmt->binding.name, stmt->binding.loc, element, stmt->isConst);
  checkLoopBody(stmt->body);
  popScope();
  flow_ = head;
  calledFunction();
}

//===----------------------------------------------------------------------===//
// Promises
//===----------------------------------------------------------------------===//

bool Sema::isHandledPromise(const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);
  if (!call)
    return false;
  // As the lint rule no-floating-promises of TypeScript says: a rejection
  // handler handles it, and finally() keeps what the Promise before it is.
  if (call->builtin == Builtin::PromiseCatch)
    return true;
  if (call->builtin == Builtin::PromiseThen)
    return call->args.size() == 2 && !call->args[1]->type->isUndefined();
  if (call->builtin == Builtin::PromiseFinally)
    return isHandledPromise(cast<MemberExpr>(call->callee)->object);
  return false;
}

void Sema::checkDiscardedPromise(const Expr *expr, bool callback, llvm::SMLoc at) {
  const Type *type = expr->type;
  if (isa<AssignExpr>(expr))
    return;
  auto *call = dyn_cast<CallExpr>(expr);
  if (call && (call->builtin == Builtin::ArrPop || call->builtin == Builtin::ArrShift ||
               call->builtin == Builtin::ArrSplice))
    return;
  if (!type->nonOptional()->isPromise()) {
    if (call && (call->builtin == Builtin::ArrMap || call->builtin == Builtin::ArrFlatMap))
      diags_.error(expr->loc(), "the Promises are not awaited, so nothing waits for them and an "
                                "error in one would end the program; wait for all of them with "
                                "'await Promise.all(...)', or keep them in a variable");
    return;
  }
  if (isHandledPromise(expr))
    return;
  if (callback) {
    diags_.error(expr->loc(), "the function returns a Promise that its caller drops, so nothing "
                              "waits for it to finish; loop with 'for (const x of xs) await "
                              "f(x)', or wait for all with 'await Promise.all(xs.map(...))'");
    return;
  }
  std::string text = statementText(at);
  if (text.empty())
    diags_.error(expr->loc(), "the Promise is not awaited, so nothing waits for it and an error in "
                              "it would end the program later; write 'await' before it, or "
                              "'void' to let it run on its own");
  else
    diags_.error(expr->loc(), "the Promise is not awaited, so nothing waits for it and an error in "
                              "it would end the program later; write 'await " +
                                  text + "', or 'void " + text + "' to let it run on its own");
}

const Type *Sema::settledResultType(const Type *value) {
  std::vector<Field> fields{{"status", Type::string(), false}};
  if (!value->isVoid() && !value->isNever())
    fields.push_back({"value", Type::optional(value), false});
  fields.push_back({"reason", Type::optional(errorClass_->type), false});
  return Type::object(std::move(fields), "PromiseSettledResult<" + value->name().str() + ">");
}

const Type *Sema::resolverType(const Type *value) {
  std::vector<const Type *> params;
  if (!value->isVoid())
    params.push_back(value);
  return Type::function(params, params.size(), false, Type::voidTy());
}

const Type *Sema::rejecterType() {
  return Type::function({errorClass_->type}, 1, false, Type::voidTy());
}

// The type of the value of `new Promise(executor)` without one written or
// expected, from what the executor gives `resolve`: nothing (void), or
// values of one type, which a check of a copy of the executor finds.
const Type *Sema::executorValueType(const Expr *executor) {
  auto *fe = dyn_cast<FunctionExpr>(executor);
  if (!fe)
    return nullptr;
  if (fe->fn->params.empty())
    return Type::voidTy();
  llvm::StringRef resolve = fe->fn->params[0].name;
  bool given = false;
  visitExprs(fe->fn->body, [&](const Expr *e) {
    auto *call = dyn_cast<CallExpr>(e);
    auto *name = call ? dyn_cast<NameExpr>(call->callee) : nullptr;
    given |= name && name->name == resolve && !call->args.empty();
  });
  if (!given)
    return Type::voidTy();
  if (fe->fn->source.empty())
    return nullptr;
  // On a copy, with `resolve` of no type, so that anything goes to it, and
  // without messages: the executor itself is checked after.
  FunctionDecl *copy = freshCopy(fe->fn);
  copy->sym = nullptr;
  auto *trial = ctx_.make<FunctionExpr>(fe->loc(), copy);
  Flow saved = flow_;
  bool muted = diags_.setMuted(true);
  Signature sig;
  sig.params = {Type::error(), Type::error()};
  sig.ret = Type::voidTy();
  sig.voidResult = true;
  sig.pad = false;
  checkFunctionExpr(trial, &sig);
  diags_.setMuted(muted);
  flow_ = std::move(saved);
  const Type *found = nullptr;
  bool unknown = false;
  visitExprs(copy->body, [&](const Expr *e) {
    auto *call = dyn_cast<CallExpr>(e);
    auto *name = call ? dyn_cast<NameExpr>(call->callee) : nullptr;
    if (!name || name->name != resolve || call->args.empty())
      return;
    const Type *t = call->args[0]->type;
    if (!t || t->isError() || t->isVoid() || t->isUndefined()) {
      unknown = true;
      return;
    }
    const Type *common = found ? unionUnify(found, t) : t;
    if (!common || common->isError())
      unknown = true;
    else
      found = common;
  });
  return unknown ? nullptr : found;
}

Expr *Sema::checkPromiseNew(NewExpr *expr, const Type *expected) {
  file_->usesAsync = true;
  auto *call = ctx_.make<CallExpr>(expr->loc(), expr, expr->args);
  call->builtin = Builtin::PromiseNew;
  call->type = Type::error();
  const Type *value = nullptr;
  if (expr->typeArgs.size() > 1) {
    diags_.error(expr->loc(), "'Promise' takes one type argument, the type of its value");
    value = Type::error();
  } else if (expr->typeArgs.size() == 1) {
    value = awaited(resolveType(expr->typeArgs[0], /*allowVoid=*/true));
  } else if (expected && expected->nonOptional()->isPromise() &&
             !expected->nonOptional()->promised()->isNever()) {
    value = expected->nonOptional()->promised();
  } else if (call->args.size() == 1) {
    value = executorValueType(call->args[0]);
    if (!value) {
      diags_.error(expr->loc(), "cannot tell the type of the Promise's value from what is given "
                                "to 'resolve'; write it, e.g. 'new Promise<int>(...)'");
      value = Type::error();
    }
  }
  if (!checkArity(call, expr->loc(), 1, 1))
    return call;
  if (value->isError()) {
    Signature loose;
    loose.params = {Type::error(), Type::error()};
    loose.pad = false;
    call->args[0] = isa<FunctionExpr>(call->args[0])
                        ? checkFunctionExpr(cast<FunctionExpr>(call->args[0]), &loose)
                        : checkExpr(call->args[0]);
    return call;
  }
  // The executor runs now, with the functions that settle the Promise.
  Signature sig;
  sig.params = {resolverType(value), rejecterType()};
  sig.minArgs = 2;
  sig.ret = Type::voidTy();
  sig.voidResult = true;
  sig.pad = false;
  call->args[0] = checkFunctionArg(call->args[0], sig, 2, "new Promise");
  calledFunction();
  call->type = Type::promise(value);
  return call;
}

// then, catch and finally.
Expr *Sema::checkPromiseMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *type = member->object->type;
  const Type *value = type->promised();
  llvm::StringRef name = member->name;
  // A Promise of a Result is a ResultAsync of neverthrow.
  static const llvm::StringLiteral resultMethods[] = {
      "map", "mapErr", "andThen", "orElse", "match", "unwrapOr", "andTee", "orTee", "andThrough"};
  if (value->isResult() && llvm::is_contained(resultMethods, name))
    return checkResultAsyncMethod(call, member, name);
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  if (rejectSpread(call))
    return call;
  file_->usesAsync = true;
  const Type *wanted = expected && expected->nonOptional()->isPromise()
                           ? expected->nonOptional()->promised()
                           : nullptr;
  // A callback: what it gets (nothing for void), and what it returns, as the
  // Promise it gives would. Null for `undefined` in its place.
  bool failed = false;
  auto callback = [&](Expr *&arg, const Type *param) -> const Type * {
    if (isa<UndefinedExpr>(arg)) {
      arg = checkExpr(arg);
      return nullptr;
    }
    Signature sig;
    sig.pad = false;
    if (param && !param->isVoid())
      sig.params = {param};
    sig.minArgs = 0;
    if (wanted && isa<FunctionExpr>(arg) && literalShaped(arg))
      sig.ret = wanted;
    arg = checkFunctionArg(arg, sig, 1, name);
    if (!arg->type->isFunction()) {
      failed = true;
      return nullptr;
    }
    // The callback runs later, so its function may outlive the call.
    if (auto *fe = dyn_cast<FunctionExpr>(arg))
      fe->fn->nonEscaping = false;
    return awaited(arg->type->ret());
  };
  // The values that two ways give, in one type: void only with void.
  auto both = [&](const Type *a, const Type *b, llvm::SMLoc loc) -> const Type * {
    if (a->isVoid() && b->isVoid())
      return a;
    if (a->isVoid() || a->isUndefined())
      return b->isVoid() ? b : Type::optional(b);
    if (b->isVoid() || b->isUndefined())
      return Type::optional(a);
    const Type *common = unify(a, b);
    if (!common || common->isError()) {
      diags_.error(loc, "'" + name + "' gives '" + a->name() + "' one way and '" + b->name() +
                            "' the other, and no type holds both; make them give one type");
      return Type::error();
    }
    return common;
  };

  const Type *result = nullptr;
  if (name == "then") {
    if (!checkArity(call, member->loc(), 1, 2))
      return call;
    call->builtin = Builtin::PromiseThen;
    const Type *onFulfilled = callback(call->args[0], value);
    const Type *onRejected =
        call->args.size() > 1 ? callback(call->args[1], errorClass_->type) : nullptr;
    if (failed)
      return call;
    const Type *fulfilled = onFulfilled ? onFulfilled : value;
    result = onRejected ? both(fulfilled, onRejected, call->loc()) : fulfilled;
  } else if (name == "catch") {
    if (!checkArity(call, member->loc(), 1, 1))
      return call;
    call->builtin = Builtin::PromiseCatch;
    const Type *onRejected = callback(call->args[0], errorClass_->type);
    if (failed)
      return call;
    result = onRejected ? both(value, onRejected, call->loc()) : value;
  } else if (name == "finally") {
    if (!checkArity(call, member->loc(), 1, 1))
      return call;
    call->builtin = Builtin::PromiseFinally;
    callback(call->args[0], nullptr);
    if (failed)
      return call;
    result = value;
  } else {
    diags_.error(member->loc(), "property '" + name + "' does not exist on type '" + type->name() +
                                    "'; a Promise has 'then', 'catch' and "
                                    "'finally'");
    checkArgsLoosely();
    return call;
  }
  if (result->isError())
    return call;
  if (result->isNever())
    result = value;
  call->type = Type::promise(result);
  return call;
}

// The promises (or values) Promise.all and the like take: an array, or an
// array literal whose elements differ, which then is a tuple. `element`
// gets the type of what each gives, `tuple` the types for a tuple.
bool Sema::checkPromiseList(Expr *&arg, const Type *wantedElement, const Type *&element,
                            std::vector<const Type *> &tuple, llvm::StringRef fn) {
  // A union that may hold a Promise, which JavaScript would wait for.
  auto mayBePromise = [&](const Type *t, llvm::SMLoc loc) {
    if (t->isPromise() || awaitedType(t) == t)
      return false;
    diags_.error(loc, "'" + fn + "' of values that may be promises ('" + t->name() +
                          "') is not supported yet; await each first, or make each a Promise");
    return true;
  };
  if (auto *lit = dyn_cast<ArrayLitExpr>(arg);
      lit && llvm::none_of(lit->elements,
                           [](const ArrayLitExpr::Element &e) { return e.spread || e.hole; })) {
    if (lit->elements.empty()) {
      if (!wantedElement) {
        diags_.error(lit->loc(), "'" + fn +
                                     "' of no promises needs the type of their values; "
                                     "give the array a type, e.g. 'const ps: Promise<int>[] = []'");
        return false;
      }
      lit->type = Type::array(Type::promise(wantedElement));
      element = wantedElement;
      return true;
    }
    std::vector<const Type *> types;
    bool same = true;
    for (ArrayLitExpr::Element &e : lit->elements) {
      e.expr = checkExpr(e.expr, wantedElement && literalShaped(e.expr) ? wantedElement : nullptr);
      if (e.expr->type->isError())
        return false;
      if (e.expr->type->isVoid()) {
        diags_.error(e.expr->loc(), "a value of type 'void' cannot be given to '" + fn + "'");
        return false;
      }
      if (mayBePromise(e.expr->type, e.expr->loc()))
        return false;
      if (!types.empty() && types.front() != e.expr->type)
        same = false;
      types.push_back(e.expr->type);
    }
    // One type that holds them all makes an array: Promise.reject() gives a
    // Promise of anything.
    const Type *common = types.front();
    for (const Type *t : types)
      if (common)
        common = unify(common, t);
    if (!same && common && !common->isError()) {
      for (ArrayLitExpr::Element &e : lit->elements)
        e.expr = convert(e.expr, common);
      types.assign(types.size(), common);
      same = true;
    }
    if (same) {
      lit->type = Type::array(types.front());
      element = awaited(types.front());
      return true;
    }
    lit->type = Type::tuple(types);
    for (const Type *t : types)
      tuple.push_back(awaited(t));
    return true;
  }
  arg = checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError())
    return false;
  if (type->isArray()) {
    if (mayBePromise(type->inner(), arg->loc()))
      return false;
    element = awaited(type->inner());
    return true;
  }
  if (type->isTuple()) {
    for (const Field &f : type->fields()) {
      if (mayBePromise(f.type, arg->loc()))
        return false;
      tuple.push_back(awaited(f.type));
    }
    return true;
  }
  // An iterator or a Set, which JavaScript would take too: an array of it.
  auto *name = dyn_cast<NameExpr>(arg);
  std::string spread = type->isIterator() || type->isDict()
                           ? "; make an array of them: '" + fn.str() + "([..." +
                                 (name ? name->name.str() : std::string("values")) + "])'"
                           : "";
  diags_.error(arg->loc(),
               "'" + fn + "' needs an array of promises, found '" + type->name() + "'" + spread);
  return false;
}

Expr *Sema::checkPromiseStatic(CallExpr *call, MemberExpr *member, const Type *expected) {
  llvm::StringRef name = member->name;
  file_->usesAsync = true;
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  if (rejectSpread(call))
    return call;
  if (!member->typeArgs.empty() && name != "withResolvers") {
    diags_.error(member->loc(), "type arguments can be given to 'Promise.withResolvers<T>()'; "
                                "the other functions of Promise take their types from what they "
                                "get");
    member->typeArgs.clear();
  }
  const Type *wanted = expected && expected->nonOptional()->isPromise()
                           ? expected->nonOptional()->promised()
                           : nullptr;
  if (wanted && wanted->isNever())
    wanted = nullptr;
  std::string fn = "Promise." + name.str();

  if (name == "resolve") {
    if (!checkArity(call, member->loc(), 0, 1))
      return call;
    call->builtin = Builtin::PromiseResolve;
    if (call->args.empty()) {
      call->type = Type::promise(Type::voidTy());
      return call;
    }
    Expr *&arg = call->args[0];
    arg = checkExpr(arg, wanted && !isa<UndefinedExpr>(arg) ? wanted : nullptr);
    const Type *type = arg->type;
    if (type->isError())
      return call;
    if (type->isPromise()) { // that Promise itself
      call->type = type;
      return call;
    }
    if (type->isUndefined()) {
      call->type = Type::promise(Type::voidTy());
      return call;
    }
    if (type->isVoid()) {
      diags_.error(arg->loc(), "a value of type 'void' cannot be given to 'Promise.resolve'");
      return call;
    }
    call->type = Type::promise(type);
    return call;
  }
  if (name == "reject") {
    if (!checkArity(call, member->loc(), 1, 1))
      return call;
    call->builtin = Builtin::PromiseReject;
    call->args[0] = checkArg(call->args[0], errorClass_->type);
    call->type = Type::promise(wanted ? wanted : Type::never());
    return call;
  }
  if (name == "withResolvers") {
    if (!checkArity(call, member->loc(), 0, 0))
      return call;
    call->builtin = Builtin::PromiseWithResolvers;
    const Type *value = nullptr;
    if (member->typeArgs.size() > 1) {
      diags_.error(member->loc(), "'Promise.withResolvers' takes one type argument, the type of "
                                  "the value");
      return call;
    }
    if (member->typeArgs.size() == 1) {
      value = awaited(resolveType(member->typeArgs[0], /*allowVoid=*/true));
    } else if (expected && expected->isObject()) {
      if (const Field *promise = expected->field("promise"); promise && promise->type->isPromise())
        value = promise->type->promised();
    }
    member->typeArgs.clear();
    if (!value) {
      diags_.error(member->loc(), "'Promise.withResolvers' needs the type of the value; write it, "
                                  "e.g. 'Promise.withResolvers<int>()'");
      return call;
    }
    if (value->isError())
      return call;
    call->type = Type::object({{"promise", Type::promise(value), false},
                               {"resolve", resolverType(value), false},
                               {"reject", rejecterType(), false}});
    return call;
  }
  static const std::pair<llvm::StringLiteral, Builtin> lists[] = {
      {"all", Builtin::PromiseAll},
      {"allSettled", Builtin::PromiseAllSettled},
      {"race", Builtin::PromiseRace},
      {"any", Builtin::PromiseAny}};
  for (const auto &[listName, builtin] : lists) {
    if (name != listName)
      continue;
    if (!checkArity(call, member->loc(), 1, 1))
      return call;
    call->builtin = builtin;
    const Type *wantedElement = nullptr;
    if (wanted && builtin == Builtin::PromiseAll && wanted->isArray())
      wantedElement = wanted->inner();
    else if (wanted && (builtin == Builtin::PromiseRace || builtin == Builtin::PromiseAny))
      wantedElement = wanted;
    const Type *element = nullptr;
    std::vector<const Type *> tuple;
    if (!checkPromiseList(call->args[0], wantedElement, element, tuple, fn))
      return call;
    if (!tuple.empty() && (builtin == Builtin::PromiseRace || builtin == Builtin::PromiseAny)) {
      // One value comes out of race and any: of one type, or of their union,
      // as in TypeScript.
      element = tuple.front();
      for (const Type *t : tuple) {
        const Type *common = unionUnify(element, t);
        if (!common || common->isError()) {
          diags_.error(call->args[0]->loc(), "'" + fn +
                                                 "' gives a value of one of the promises, "
                                                 "but they give '" +
                                                 element->name() + "' and '" + t->name() +
                                                 "'; make them give one type");
          return call;
        }
        element = common;
      }
    }
    if (element && element->isNever())
      element = wantedElement ? wantedElement : Type::voidTy();
    switch (builtin) {
    case Builtin::PromiseAll:
      if (!tuple.empty()) {
        for (const Type *t : tuple)
          if (t->isVoid()) {
            diags_.error(call->args[0]->loc(), "'Promise.all' collects the values, and a "
                                               "Promise<void> has none; await it on its own");
            return call;
          }
        call->type = Type::promise(Type::tuple(tuple));
      } else if (element->isVoid()) {
        // Nothing to collect (JavaScript gives an array of undefined, which
        // tz has no type for): it only waits.
        call->type = Type::promise(Type::voidTy());
      } else {
        call->type = Type::promise(Type::array(element));
      }
      break;
    case Builtin::PromiseAllSettled:
      if (!tuple.empty()) {
        // A tuple of results, each of its own type, as in TypeScript.
        std::vector<const Type *> results;
        for (const Type *t : tuple)
          results.push_back(settledResultType(t->isNever() ? Type::voidTy() : t));
        call->type = Type::promise(Type::tuple(results));
      } else {
        call->type = Type::promise(Type::array(settledResultType(element)));
      }
      break;
    default:
      call->type = Type::promise(element);
      break;
    }
    return call;
  }
  diags_.error(member->loc(), "'Promise." + name +
                                  "' is not supported; the functions of Promise "
                                  "are 'resolve', 'reject', 'all', 'allSettled', 'race', 'any' and "
                                  "'withResolvers'");
  checkArgsLoosely();
  return call;
}

//===----------------------------------------------------------------------===//
// Timers and the queues
//===----------------------------------------------------------------------===//

// A callback that runs later with nothing: `() => void`; an async one is let
// run on its own, as a listener is (docs/events.md).
Expr *Sema::checkTaskCallback(Expr *arg, llvm::StringRef fn) {
  Signature sig;
  sig.ret = Type::voidTy();
  sig.voidResult = true;
  sig.asyncVoid = true;
  const Type *task = Type::function({}, 0, false, Type::voidTy());
  if (isa<FunctionExpr>(arg)) {
    arg = checkFunctionExpr(cast<FunctionExpr>(arg), &sig);
    return arg;
  }
  arg = checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError())
    return arg;
  if (!type->isFunction()) {
    diags_.error(arg->loc(), "'" + fn + "' needs a function, found '" + type->name() + "'");
    return arg;
  }
  if (type == task)
    return arg;
  if (type->minArgs() == 0 && !type->hasRest())
    if (Expr *adapted = adaptFunction(arg, task))
      return adapted;
  diags_.error(arg->loc(), "'" + fn + "' calls its callback with nothing, but this function is '" +
                               type->name() + "'; pass an arrow that calls it, e.g. '() => f()'");
  return arg;
}

Expr *Sema::checkAsyncGlobal(CallExpr *call, NameExpr *name) {
  static const llvm::StringLiteral names[] = {"setTimeout",    "setInterval",   "setImmediate",
                                              "clearTimeout",  "clearInterval", "clearImmediate",
                                              "queueMicrotask"};
  if (!llvm::any_of(names, [&](llvm::StringRef n) { return isGlobalName(name, n); }))
    return nullptr;
  file_->usesAsync = true;
  call->type = Type::error();
  llvm::StringRef fn = name->name;
  if (rejectSpread(call))
    return call;
  if (fn == "setTimeout" || fn == "setInterval") {
    if (call->args.size() > 2) {
      diags_.error(call->args[2]->loc(), "arguments for the callback are not supported; pass an "
                                         "arrow that calls it with them");
      for (Expr *&arg : call->args)
        arg = checkExpr(arg);
      return call;
    }
    if (!checkArity(call, name->loc(), 1, 2))
      return call;
    call->builtin = fn == "setTimeout" ? Builtin::SetTimeout : Builtin::SetInterval;
    call->args[0] = checkTaskCallback(call->args[0], fn);
    if (call->args.size() > 1)
      call->args[1] = checkArg(call->args[1], Type::doubleTy());
    call->type = Type::timeout();
    return call;
  }
  if (fn == "setImmediate" || fn == "queueMicrotask") {
    if (call->args.size() > 1 && fn == "setImmediate") {
      diags_.error(call->args[1]->loc(), "arguments for the callback are not supported; pass an "
                                         "arrow that calls it with them");
      for (Expr *&arg : call->args)
        arg = checkExpr(arg);
      return call;
    }
    if (!checkArity(call, name->loc(), 1, 1))
      return call;
    call->builtin = fn == "setImmediate" ? Builtin::SetImmediate : Builtin::QueueMicrotask;
    call->args[0] = checkTaskCallback(call->args[0], fn);
    call->type = fn == "setImmediate" ? Type::immediate() : Type::voidTy();
    return call;
  }
  // clearTimeout, clearInterval and clearImmediate.
  if (!checkArity(call, name->loc(), 0, 1))
    return call;
  bool immediate = fn == "clearImmediate";
  call->builtin = immediate ? Builtin::ClearImmediate : Builtin::ClearTimeout;
  if (call->args.empty()) { // does nothing, as in Node
    auto *none = ctx_.make<UndefinedExpr>(call->loc());
    none->type = Type::undefinedTy();
    call->args.push_back(none);
  }
  call->args[0] =
      checkArg(call->args[0], Type::optional(immediate ? Type::immediate() : Type::timeout()));
  call->type = Type::voidTy();
  return call;
}

const Type *Sema::timerCallType(const Expr *init) const {
  auto *call = dyn_cast<CallExpr>(init);
  const Expr *callee = call ? call->callee : nullptr;
  if (!callee)
    return nullptr;
  if (isGlobalName(callee, "setTimeout") || isGlobalName(callee, "setInterval"))
    return Type::timeout();
  if (isGlobalName(callee, "setImmediate"))
    return Type::immediate();
  return nullptr;
}

//===----------------------------------------------------------------------===//
// ResultAsync of neverthrow
//===----------------------------------------------------------------------===//

namespace {

// The Result a Promise of one that is expected holds.
const Type *wantedResult(const Type *expected) {
  const Type *type = expected ? expected->nonOptional() : nullptr;
  return type && type->isPromise() && type->promised()->isResult() ? type->promised() : nullptr;
}

// What a callback gets for a half of a Result: `ok()` holds undefined.
const Type *halfOf(const Type *half) { return half->isVoid() ? Type::undefinedTy() : half; }

} // namespace

Expr *Sema::checkResultAsyncCall(CallExpr *call, NameExpr *name, llvm::StringRef which,
                                 const Type *expected) {
  file_->usesAsync = true;
  if (which == "fromPromise" || which == "fromSafePromise")
    return checkResultFromPromise(call, name->loc(), which == "fromSafePromise", expected);
  if (which == "fromAsyncThrowable")
    return checkResultAsyncThrowable(call, name->loc());
  // okAsync(v) and errAsync(e): ok(v) and err(e) in a Promise settled now.
  Expr *made = checkOkErr(call, name, which == "okAsync", wantedResult(expected));
  if (!made->type->isResult())
    return made;
  auto *resolve = ctx_.make<CallExpr>(call->loc(), call->callee, std::vector<Expr *>{made});
  resolve->builtin = Builtin::PromiseResolve;
  resolve->type = Type::promise(made->type);
  return resolve;
}

Expr *Sema::checkResultAsyncStatic(CallExpr *call, MemberExpr *member, const Type *expected) {
  auto *name = dyn_cast<NameExpr>(member->object);
  if (!name || call->optional || member->optional || neverthrowName(name->name) != "ResultAsync")
    return nullptr;
  file_->usesAsync = true;
  llvm::StringRef method = member->name;
  if (method == "fromPromise" || method == "fromSafePromise")
    return checkResultFromPromise(call, member->loc(), method == "fromSafePromise", expected);
  if (method == "fromThrowable")
    return checkResultAsyncThrowable(call, member->loc());
  std::string hint;
  if (method == "combine" || method == "combineWithAllErrors")
    hint = "; await them with 'Promise.all' and use 'Result." + method.str() + "'";
  diags_.error(member->loc(), "'" + name->name + "." + method +
                                  "' is not supported; ResultAsync has 'fromPromise', "
                                  "'fromSafePromise' and 'fromThrowable'" +
                                  hint);
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  return call;
}

// fromPromise(p, toError) and fromSafePromise(p): an Ok of what p gives, or
// an Err of what toError makes of its error (fromSafePromise lets the error
// reject the ResultAsync).
Expr *Sema::checkResultFromPromise(CallExpr *call, llvm::SMLoc loc, bool safe,
                                   const Type *expected) {
  call->type = Type::error();
  llvm::StringRef fn = safe ? "fromSafePromise" : "fromPromise";
  if (rejectSpread(call) || !checkArity(call, loc, safe ? 1 : 2, safe ? 1 : 2))
    return call;
  const Type *wanted = wantedResult(expected);
  Expr *&promise = call->args[0];
  const Type *wantedValue = wanted && !wanted->okType()->isNever() ? wanted->okType() : nullptr;
  promise = checkExpr(promise, wantedValue && (isa<NewExpr>(promise) || isa<CallExpr>(promise))
                                   ? Type::promise(wantedValue)
                                   : nullptr);
  if (promise->type->isError())
    return call;
  if (!promise->type->isPromise()) {
    diags_.error(promise->loc(),
                 "'" + fn + "' takes a Promise, found '" + promise->type->name() + "'");
    return call;
  }
  const Type *value = promise->type->promised(), *error = Type::never();
  if (!safe) {
    Signature sig;
    sig.params = {errorClass_->type};
    sig.pad = false;
    call->args[1] = checkFunctionArg(call->args[1], sig, 1, fn);
    if (!call->args[1]->type->isFunction())
      return call;
    if (auto *fe = dyn_cast<FunctionExpr>(call->args[1]))
      fe->fn->nonEscaping = false; // it runs later
    error = call->args[1]->type->ret();
    if (error->isVoid()) {
      diags_.error(call->args[1]->loc(), "the function that turns the Error into the error of the "
                                         "Result must return it");
      return call;
    }
  }
  // The halves of the type expected, where they go.
  if (wanted && isImplicitlyConvertible(value, wanted->okType()) &&
      isImplicitlyConvertible(error, wanted->errType())) {
    value = wanted->okType();
    error = wanted->errType();
  }
  call->builtin = safe ? Builtin::ResultAsyncFromSafePromise : Builtin::ResultAsyncFromPromise;
  call->type = Type::promise(Type::result(value, error));
  return call;
}

// ResultAsync.fromThrowable(f, toError?): a function with the parameters of
// f, which gives a ResultAsync of what f's Promise gives, or of what f throws
// or its Promise is rejected with, as toError makes it.
Expr *Sema::checkResultAsyncThrowable(CallExpr *call, llvm::SMLoc loc) {
  call->type = Type::error();
  if (rejectSpread(call) || !checkArity(call, loc, 1, 2))
    return call;
  Expr *&fn = call->args[0];
  fn = checkExpr(fn);
  const Type *fnType = fn->type;
  if (fnType->isError())
    return call;
  if (!fnType->isFunction() || !fnType->ret()->isPromise()) {
    diags_.error(fn->loc(), "'fromThrowable' of ResultAsync takes a function that gives a Promise, "
                            "found '" +
                                fnType->name() + "'");
    return call;
  }
  const Type *error = errorClass_->type;
  if (call->args.size() > 1) {
    Signature sig;
    sig.params = {errorClass_->type};
    sig.minArgs = 1;
    Expr *&toError = call->args[1];
    toError = isa<FunctionExpr>(toError) ? checkFunctionExpr(cast<FunctionExpr>(toError), &sig)
                                         : checkExpr(toError);
    const Type *toType = toError->type;
    if (toType->isError())
      return call;
    if (!toType->isFunction() || toType->minArgs() > 1 ||
        (!toType->params().empty() && toType->params()[0] != errorClass_->type)) {
      diags_.error(toError->loc(), "the second argument of 'fromThrowable' turns the Error into "
                                   "the error of the Result: a function that takes an Error, e.g. "
                                   "'(e) => e.message'");
      return call;
    }
    error = toType->ret();
    if (error->isVoid()) {
      diags_.error(toError->loc(), "the function that turns the Error into the error of the "
                                   "Result must return it");
      return call;
    }
  }
  call->builtin = Builtin::ResultAsyncFromThrowable;
  call->type = Type::function(fnType->params(), fnType->minArgs(), fnType->hasRest(),
                              Type::promise(Type::result(fnType->ret()->promised(), error)));
  return call;
}

Expr *Sema::checkResultAsyncMethod(CallExpr *call, MemberExpr *member, llvm::StringRef name) {
  call->type = Type::error();
  file_->usesAsync = true;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  if (rejectSpread(call))
    return call;
  // asyncMap and asyncAndThen of a Result: map and andThen of a ResultAsync
  // as to types, but the function runs now and must give a promise.
  llvm::StringRef given = name;
  bool ofResult = member->object->type->isResult();
  if (ofResult)
    name = name == "asyncMap" ? "map" : "andThen";
  const Type *result = ofResult ? member->object->type : member->object->type->promised();
  const Type *value = result->okType(), *error = result->errType();
  // A callback: what it gets, and what it gives, as `await` would.
  auto callback = [&](Expr *&arg, const Type *half) -> const Type * {
    Signature sig;
    sig.pad = false;
    sig.params = {halfOf(half)};
    arg = checkFunctionArg(arg, sig, 1, given);
    if (!arg->type->isFunction())
      return nullptr;
    if (auto *fe = dyn_cast<FunctionExpr>(arg))
      fe->fn->nonEscaping = false; // it runs later
    if (ofResult && !arg->type->ret()->isPromise()) {
      diags_.error(arg->loc(), "the function of '" + given + "' must give a " +
                                   (given == "asyncMap" ? "Promise" : "ResultAsync") + ", found '" +
                                   arg->type->ret()->name() + "'; use '" + name +
                                   "' for a function that gives " +
                                   (given == "asyncMap" ? "a value" : "a Result"));
      return nullptr;
    }
    return awaited(arg->type->ret());
  };
  if (name == "andThrough") {
    diags_.error(member->loc(), "'andThrough' of a ResultAsync is not supported yet; use "
                                "'andThen', which gives the Result its function gives");
    if (call->args.size() == 1)
      callback(call->args[0], value); // for the errors in it
    else
      checkArgsLoosely();
    return call;
  }
  struct Method {
    llvm::StringLiteral name;
    Builtin builtin;
    unsigned args;
  };
  static const Method methods[] = {
      {"map", Builtin::ResultAsyncMap, 1},         {"mapErr", Builtin::ResultAsyncMapErr, 1},
      {"andThen", Builtin::ResultAsyncAndThen, 1}, {"orElse", Builtin::ResultAsyncOrElse, 1},
      {"match", Builtin::ResultAsyncMatch, 2},     {"unwrapOr", Builtin::ResultAsyncUnwrapOr, 1},
      {"andTee", Builtin::ResultAsyncAndTee, 1},   {"orTee", Builtin::ResultAsyncOrTee, 1}};
  const Method *method = nullptr;
  for (const Method &m : methods)
    if (m.name == name)
      method = &m;
  if (!method || !checkArity(call, member->loc(), method->args, method->args))
    return call;
  call->builtin = method->builtin;
  const Type *type = nullptr;
  switch (method->builtin) {
  case Builtin::ResultAsyncMap:
  case Builtin::ResultAsyncMapErr: {
    bool onError = method->builtin == Builtin::ResultAsyncMapErr;
    const Type *made = callback(call->args[0], onError ? error : value);
    if (!made)
      return call;
    type = onError ? Type::result(value, made) : Type::result(made, error);
    break;
  }
  case Builtin::ResultAsyncAndThen:
  case Builtin::ResultAsyncOrElse: {
    bool onError = method->builtin == Builtin::ResultAsyncOrElse;
    const Type *made = callback(call->args[0], onError ? error : value);
    if (!made)
      return call;
    if (!made->isResult()) {
      diags_.error(call->args[0]->loc(),
                   "the function of '" + given + "' must give a " +
                       (ofResult ? "ResultAsync" : "Result or a ResultAsync") + ", found '" +
                       call->args[0]->type->ret()->name() + "'; to give a value, use '" +
                       (onError    ? "mapErr"
                        : ofResult ? "asyncMap"
                                   : "map") +
                       "'");
      return call;
    }
    // The halves the other way meet those of the Result the function gives.
    const Type *kept = onError ? made->errType() : made->okType();
    const Type *mine = onError ? value : error;
    const Type *theirs = onError ? made->okType() : made->errType();
    const Type *other = unifyHalf(mine, theirs);
    if (!other) {
      diags_.error(call->args[0]->loc(),
                   std::string("the ") + (onError ? "values" : "errors") + " '" +
                       mine->name().str() + "' and '" + theirs->name().str() +
                       "' do not go into one type; make them one, e.g. with '" +
                       (onError ? "map" : "mapErr") + "'");
      return call;
    }
    type = onError ? Type::result(other, kept) : Type::result(kept, other);
    break;
  }
  case Builtin::ResultAsyncMatch: {
    const Type *a = callback(call->args[0], value), *b = callback(call->args[1], error);
    if (!a || !b)
      return call;
    bool voids = a->isVoid() && b->isVoid();
    const Type *both = voids ? a : a->isVoid() || b->isVoid() ? nullptr : unify(a, b);
    if (!both || both->isError()) {
      diags_.error(call->loc(), "'match' gives '" + a->name() + "' for an Ok and '" + b->name() +
                                    "' for an Err, and no type holds both");
      return call;
    }
    call->type = Type::promise(both);
    return call;
  }
  case Builtin::ResultAsyncUnwrapOr: {
    Expr *&arg = call->args[0];
    arg = checkExpr(arg, !value->isNever() && !value->isVoid() ? value : nullptr);
    const Type *both = arg->type->isError() ? nullptr : unify(value, arg->type);
    if (!both || both->isError()) {
      if (!arg->type->isError())
        diags_.error(arg->loc(), "'unwrapOr' gives '" + value->name() + "' for an Ok and '" +
                                     arg->type->name() + "' otherwise, and no type holds both");
      return call;
    }
    arg = convert(arg, both);
    call->type = Type::promise(both);
    return call;
  }
  default: // andTee and orTee: what the function gives is dropped
    if (!callback(call->args[0], method->builtin == Builtin::ResultAsyncOrTee ? error : value))
      return call;
    type = result;
    break;
  }
  call->type = Type::promise(type);
  return call;
}

Expr *Sema::checkNextTick(CallExpr *call, MemberExpr *member) {
  file_->usesAsync = true;
  call->type = Type::error();
  if (call->args.size() > 1) {
    diags_.error(call->args[1]->loc(), "arguments for the callback are not supported; pass an "
                                       "arrow that calls it with them");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  if (!checkArity(call, member->loc(), 1, 1))
    return call;
  call->builtin = Builtin::NextTick;
  call->args[0] = checkTaskCallback(call->args[0], "process.nextTick");
  call->type = Type::voidTy();
  return call;
}

// ref(), unref(), hasRef() and refresh() of a Timeout or an Immediate.
Expr *Sema::checkTimerMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  call->type = Type::error();
  struct Method {
    llvm::StringLiteral name;
    Builtin builtin;
  };
  static const Method methods[] = {{"ref", Builtin::TimerRef},
                                   {"unref", Builtin::TimerUnref},
                                   {"hasRef", Builtin::TimerHasRef},
                                   {"refresh", Builtin::TimerRefresh}};
  const Method *method = nullptr;
  for (const Method &m : methods)
    if (m.name == name && (m.builtin != Builtin::TimerRefresh || !type->isImmediate()))
      method = &m;
  if (!method) {
    diags_.error(member->loc(), "property '" + name + "' does not exist on type '" + type->name() +
                                    "'; it has 'ref', 'unref', 'hasRef'" +
                                    (type->isImmediate() ? "" : " and 'refresh'"));
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  if (rejectSpread(call) || !checkArity(call, member->loc(), 0, 0))
    return call;
  call->builtin = method->builtin;
  call->type = method->builtin == Builtin::TimerHasRef ? Type::boolTy() : type;
  return call;
}

} // namespace tz
