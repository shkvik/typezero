#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/stmt/loop_checker/loop_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/loop_scanner/loop_scanner.h"
#include "compiler/check/flow/loop_bounds/loop_bounds.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/iterators/iterable_checker/iterable_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/library/regex/match_checker/match_checker.h"
#include "compiler/check/shared_helpers/shared_helpers.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

const Type *literalType(const Expr *value);

namespace {

// What `await` of a value of `type` gives: the value of a Promise, or the
// value itself.
const Type *awaited(const Type *type) { return type->isPromise() ? type->promised() : type; }

// The text of a statement that starts at `at`, for hints: up to the end of
// its line, without a `;` or a comment at the end.
std::string statementText(llvm::SMLoc at) {
  if (!at.isValid()) {
    return "";
  }
  const char *start = at.getPointer(), *end = start;
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

} // namespace

AsyncChecker::AsyncChecker(Sema &sema) : sema_(sema) {}

const Type *AsyncChecker::resolvePromiseType(const TypeRef &ref) {
  if (ref.name != "Promise" && ref.name != "PromiseLike") {
    return nullptr;
  }
  if (ref.args.size() != 1) {
    sema_.diags_.error(ref.loc, "'" + ref.name +
                              "' takes one type argument, the type of its value, e.g. '" +
                              ref.name + "<int>'");
    return Type::error();
  }
  const Type *value = sema_.typeResolver().resolveType(ref.args[0], /*allowVoid=*/true);

  // A Promise of a Promise is a Promise of its value in JavaScript.
  return Type::promise(awaited(value));
}

const Type *AsyncChecker::asyncReturnType(const FunctionDecl *fn, const Type *declared) {
  if (!fn->isAsync || fn->isGenerator || declared->isError() || declared->isPromise()) {
    return declared;
  }
  sema_.diags_.error(fn->returnType->loc, "an async function returns a Promise: declare its type as "
                                    "'Promise<" +
                                        declared->name().str() + ">'");

  return Type::promise(declared);
}

const Type *AsyncChecker::inferAsyncType(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
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
    const Type *common = type ? TypeRelations::unify(type, t) : t;
    if (!common || common->isError()) {
      if (report) {
        sema_.diags_.error(ret->value->loc(), quotedName(fn->name) + " returns '" + t->name() +
                                            "' here and '" + type->name() +
                                            "' elsewhere; a function returns values of one type");
      }
      return Type::error();
    }
    type = common;
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
      // `return p`: the Promise of the function follows p.
      if (t->promised() != value && !t->promised()->isNever()) {
        sema_.diags_.error(ret->value->loc(), "this Promise gives '" + t->promised()->name() + "', but " +
                                            quotedName(fn->name) + " gives '" + value->name() +
                                            "'; await it and convert the value");
      }
      continue;
    }
    if (!value->isVoid()) {
      ret->value = sema_.typeRelations().convert(ret->value, value);
    }
  }
}

Expr *AsyncChecker::checkAwait(AwaitExpr *await, const Type *expected) {
  await->type = Type::error();
  sema_.file_->usesAsync = true;
  FuncSymbol *fn = sema_.currentFunction_;
  if (fn && !(fn->decl && fn->decl->isAsync)) {
    FunctionDecl *decl = fn->decl;
    std::string hint = decl && decl->isArrow         ? "'async (...) => ...'"
                       : decl && decl->cls           ? "'async " + decl->name.str() + "() { ... }'"
                       : decl && !decl->name.empty() ? "'async function " + decl->name.str() + "'"
                                                     : "'async function'";
    sema_.diags_.error(await->loc(), "'await' can only be used in an async function or in top-level "
                               "code; make the function async: " +
                                   hint);
  } else if (!fn) {
    sema_.file_->topLevelAwait = true;
  }
  // `await new Promise(...)` where an int is expected waits for a
  // Promise<int>.
  const Type *hint = nullptr;
  if (expected && !expected->isError() && !expected->isVoid() &&
      (isa<NewExpr>(await->value) || isa<CallExpr>(await->value))) {
    hint = Type::promise(expected);
  }
  await->value = sema_.exprChecker().checkExpr(await->value, hint);
  const Type *type = await->value->type;
  // Other tasks run while this one waits: they may change anything.
  sema_.flowChecker().calledFunction();
  if (type->isError()) {
    return await;
  }
  const Type *result = awaitedType(type);
  if (!result) {
    sema_.diags_.error(await->value->loc(),
                 "'await' of '" + type->name() +
                     "' gives the value of the Promise or the value itself, which make no one "
                     "type; check what it holds first");
    return await;
  }
  await->type = result;

  return await;
}

const Type *AsyncChecker::awaitedType(const Type *type) {
  if (type->isPromise()) {
    return type->promised();
  }
  if (type->isOptional()) {
    const Type *inner = awaitedType(type->inner());
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

// `for await (const x of xs)` (docs/iterators.md, step 4): over the values
// of an async iterator, the lines of readline, or the elements of an array
// or an iterator, each awaited. It waits, as `await` does, so it goes where
// `await` goes.
void AsyncChecker::checkForAwait(ForOfStmt *stmt) {
  sema_.file_->usesAsync = true;
  FuncSymbol *fn = sema_.currentFunction_;
  if (fn && !(fn->decl && fn->decl->isAsync)) {
    sema_.diags_.error(stmt->loc(), "'for await' can only be used in an async function or in top-level "
                              "code; make the function async");
  } else if (!fn) {
    sema_.file_->topLevelAwait = true;
  }
  sema_.iteratorMethods().markFused(stmt->iterable);
  stmt->iterable = sema_.genericChecker().apparent(sema_.exprChecker().checkExpr(stmt->iterable));
  // An object with `[Symbol.asyncIterator]()` gives what that gives; one
  // with `[Symbol.iterator]()`, its iterator.
  if (Expr *iterator = sema_.iterableChecker().asyncUserIterator(stmt->iterable)) {
    stmt->iterable = iterator;
  } else {
    stmt->iterable = sema_.dictChecker().iterableOf(stmt->iterable);
  }
  if (stmt->iterable->type->isMatch()) { // the captures
    stmt->iterable = sema_.matchChecker().matchItems(stmt->iterable);
  }
  const Type *type = stmt->iterable->type;
  const Type *element = sema_.generatorChecker().asyncIteratedType(type);
  if (!element) {
    if (type->isOptional() && sema_.generatorChecker().asyncIteratedType(type->inner())) {
      sema_.diags_.error(stmt->iterable->loc(),
                   "'" + type->name() + "' may be undefined; check it before iterating");
    } else if (type->isRecord()) {
      sema_.diags_.error(stmt->iterable->loc(),
                   "a Record is not iterable, as an object is not in JavaScript; walk its keys "
                   "with 'for...in', or its entries with 'for (const [k, v] of "
                   "Object.entries(r))'");
    } else if (!type->isError()) {
      sema_.diags_.error(stmt->iterable->loc(), "'for await' needs an async iterator, an array or an "
                                          "iterator, found '" +
                                              type->name() + "'");
    }
    element = Type::error();
  } else if (type->isIterator()) {
    sema_.generatorChecker().checkSendsNothing(type, stmt->iterable->loc(), "'for await'");
  }
  if (stmt->binding.typeAnn) {
    sema_.diags_.error(stmt->binding.typeAnn->loc,
                 "a 'for await' variable cannot have a type annotation");
  }
  if (stmt->binding.destructured && !stmt->binding.objectPattern && !element->isArray() &&
      !element->isTuple() && !element->isMatch() && !element->isError()) {
    sema_.diags_.error(stmt->binding.loc,
                 "only arrays can be destructured; the elements are '" + element->name() + "'");
    element = Type::error();
  }
  // Other tasks run while it waits: they may change anything, as the
  // iterator itself may.
  sema_.flowChecker().calledFunction();
  Sema::LoopScan scan = sema_.loopScanner().scanLoop({stmt->body}, {});
  scan.callsFunctions = true;
  stmt->borrowElements = false;
  Sema::Flow head = sema_.loopBounds().loopHead(scan, nullptr, /*induction=*/false);
  sema_.flow_ = head;
  sema_.pushScope();
  stmt->binding.sym = sema_.scopes().declareVar(stmt->binding.name, stmt->binding.loc, element, stmt->isConst);
  sema_.loopChecker().checkLoopBody(stmt->body);
  sema_.popScope();
  sema_.flow_ = head;
  sema_.flowChecker().calledFunction();
}

bool AsyncChecker::isHandledPromise(const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);
  if (!call) {
    return false;
  }
  // As the lint rule no-floating-promises of TypeScript says: a rejection
  // handler handles it, and finally() keeps what the Promise before it is.
  if (call->builtin == Builtin::PromiseCatch) {
    return true;
  }
  if (call->builtin == Builtin::PromiseThen) {
    return call->args.size() == 2 && !call->args[1]->type->isUndefined();
  }
  if (call->builtin == Builtin::PromiseFinally) {
    return isHandledPromise(cast<MemberExpr>(call->callee)->object);
  }

  return false;
}

void AsyncChecker::checkDiscardedPromise(const Expr *expr, bool callback, llvm::SMLoc at) {
  const Type *type = expr->type;
  if (isa<AssignExpr>(expr)) {
    return;
  }
  auto *call = dyn_cast<CallExpr>(expr);
  if (call && (call->builtin == Builtin::ArrPop || call->builtin == Builtin::ArrShift ||
               call->builtin == Builtin::ArrSplice)) {
    return;
  }
  if (!type->nonOptional()->isPromise()) {
    if (call && (call->builtin == Builtin::ArrMap || call->builtin == Builtin::ArrFlatMap)) {
      sema_.diags_.error(expr->loc(), "the Promises are not awaited, so nothing waits for them and an "
                                "error in one would end the program; wait for all of them with "
                                "'await Promise.all(...)', or keep them in a variable");
    }
    return;
  }
  if (isHandledPromise(expr)) {
    return;
  }
  if (callback) {
    sema_.diags_.error(expr->loc(), "the function returns a Promise that its caller drops, so nothing "
                              "waits for it to finish; loop with 'for (const x of xs) await "
                              "f(x)', or wait for all with 'await Promise.all(xs.map(...))'");
    return;
  }
  std::string text = statementText(at);
  if (text.empty()) {
    sema_.diags_.error(expr->loc(), "the Promise is not awaited, so nothing waits for it and an error in "
                              "it would end the program later; write 'await' before it, or "
                              "'void' to let it run on its own");
  } else {
    sema_.diags_.error(expr->loc(), "the Promise is not awaited, so nothing waits for it and an error in "
                              "it would end the program later; write 'await " +
                                  text + "', or 'void " + text + "' to let it run on its own");
  }
}

} // namespace tz
