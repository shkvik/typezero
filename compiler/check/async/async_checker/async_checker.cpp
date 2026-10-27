#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/loop_bounds/loop_bounds.h"
#include "compiler/check/flow/loop_scanner/loop_scanner.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/iterators/iterable_checker/iterable_checker.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/shared_helpers/shared_helpers.h"
#include "compiler/check/stmt/loop_checker/loop_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/library/regex/match_checker/match_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

const Type *literalType(const Expr *value);

namespace {

const Type *awaited(const Type *type) { return type->isPromise() ? type->promised() : type; }

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

}

AsyncChecker::AsyncChecker(Checker &checker) : checker_(checker) {}

const Type *AsyncChecker::resolvePromiseType(const TypeRef &ref) {
  if (ref.name != "Promise" && ref.name != "PromiseLike") {
    return nullptr;
  }
  if (ref.args.size() != 1) {
    checker_.diags_.error(ref.loc, "'" + ref.name +
                                       "' takes one type argument, the type of its value, e.g. '" +
                                       ref.name + "<int>'");
    return Type::error();
  }
  const Type *value = checker_.typeResolver().resolveType(ref.args[0], true);

  return Type::promise(awaited(value));
}

const Type *AsyncChecker::asyncReturnType(const FunctionDecl *fn, const Type *declared) {
  if (!fn->isAsync || fn->isGenerator || declared->isError() || declared->isPromise()) {
    return declared;
  }
  checker_.diags_.error(fn->returnType->loc,
                        "an async function returns a Promise: declare its type as "
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
        checker_.diags_.error(ret->value->loc(),
                              quotedName(fn->name) + " returns '" + t->name() + "' here and '" +
                                  type->name() +
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
      if (t->promised() != value && !t->promised()->isNever()) {
        checker_.diags_.error(ret->value->loc(), "this Promise gives '" + t->promised()->name() +
                                                     "', but " + quotedName(fn->name) + " gives '" +
                                                     value->name() +
                                                     "'; await it and convert the value");
      }
      continue;
    }
    if (!value->isVoid()) {
      ret->value = checker_.typeRelations().convert(ret->value, value);
    }
  }
}

Expr *AsyncChecker::checkAwait(AwaitExpr *await, const Type *expected) {
  await->type = Type::error();
  checker_.file_->usesAsync = true;
  FuncSymbol *fn = checker_.currentFunction_;
  if (fn && !(fn->decl && fn->decl->isAsync)) {
    FunctionDecl *decl = fn->decl;
    std::string hint = decl && decl->isArrow         ? "'async (...) => ...'"
                       : decl && decl->cls           ? "'async " + decl->name.str() + "() { ... }'"
                       : decl && !decl->name.empty() ? "'async function " + decl->name.str() + "'"
                                                     : "'async function'";
    checker_.diags_.error(await->loc(),
                          "'await' can only be used in an async function or in top-level "
                          "code; make the function async: " +
                              hint);
  } else if (!fn) {
    checker_.file_->topLevelAwait = true;
  }
  const Type *hint = nullptr;
  if (expected && !expected->isError() && !expected->isVoid() &&
      (isa<NewExpr>(await->value) || isa<CallExpr>(await->value))) {
    hint = Type::promise(expected);
  }
  await->value = checker_.exprChecker().checkExpr(await->value, hint);
  const Type *type = await->value->type;
  checker_.flowChecker().calledFunction();
  if (type->isError()) {
    return await;
  }
  const Type *result = awaitedType(type);
  if (!result) {
    checker_.diags_.error(
        await->value->loc(),
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

void AsyncChecker::checkForAwait(ForOfStmt *stmt) {
  checker_.file_->usesAsync = true;
  FuncSymbol *fn = checker_.currentFunction_;
  if (fn && !(fn->decl && fn->decl->isAsync)) {
    checker_.diags_.error(stmt->loc(),
                          "'for await' can only be used in an async function or in top-level "
                          "code; make the function async");
  } else if (!fn) {
    checker_.file_->topLevelAwait = true;
  }
  checker_.iteratorMethods().markFused(stmt->iterable);
  stmt->iterable =
      checker_.genericChecker().apparent(checker_.exprChecker().checkExpr(stmt->iterable));
  if (Expr *iterator = checker_.iterableChecker().asyncUserIterator(stmt->iterable)) {
    stmt->iterable = iterator;
  } else {
    stmt->iterable = checker_.dictChecker().iterableOf(stmt->iterable);
  }
  if (stmt->iterable->type->isMatch()) {
    stmt->iterable = checker_.matchChecker().matchItems(stmt->iterable);
  }
  const Type *type = stmt->iterable->type;
  const Type *element = checker_.generatorChecker().asyncIteratedType(type);
  if (!element) {
    if (type->isOptional() && checker_.generatorChecker().asyncIteratedType(type->inner())) {
      checker_.diags_.error(stmt->iterable->loc(),
                            "'" + type->name() + "' may be undefined; check it before iterating");
    } else if (type->isRecord()) {
      checker_.diags_.error(
          stmt->iterable->loc(),
          "a Record is not iterable, as an object is not in JavaScript; walk its keys "
          "with 'for...in', or its entries with 'for (const [k, v] of "
          "Object.entries(r))'");
    } else if (!type->isError()) {
      checker_.diags_.error(stmt->iterable->loc(),
                            "'for await' needs an async iterator, an array or an "
                            "iterator, found '" +
                                type->name() + "'");
    }
    element = Type::error();
  } else if (type->isIterator()) {
    checker_.generatorChecker().checkSendsNothing(type, stmt->iterable->loc(), "'for await'");
  }
  if (stmt->binding.typeAnn) {
    checker_.diags_.error(stmt->binding.typeAnn->loc,
                          "a 'for await' variable cannot have a type annotation");
  }
  if (stmt->binding.destructured && !stmt->binding.objectPattern && !element->isArray() &&
      !element->isTuple() && !element->isMatch() && !element->isError()) {
    checker_.diags_.error(stmt->binding.loc, "only arrays can be destructured; the elements are '" +
                                                 element->name() + "'");
    element = Type::error();
  }
  checker_.flowChecker().calledFunction();
  Checker::LoopScan scan = checker_.loopScanner().scanLoop({stmt->body}, {});
  scan.callsFunctions = true;
  stmt->borrowElements = false;
  Checker::Flow head = checker_.loopBounds().loopHead(scan, nullptr, false);
  checker_.flow_ = head;
  checker_.pushScope();
  stmt->binding.sym =
      checker_.scopes().declareVar(stmt->binding.name, stmt->binding.loc, element, stmt->isConst);
  checker_.loopChecker().checkLoopBody(stmt->body);
  checker_.popScope();
  checker_.flow_ = head;
  checker_.flowChecker().calledFunction();
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
      checker_.diags_.error(expr->loc(),
                            "the Promises are not awaited, so nothing waits for them and an "
                            "error in one would end the program; wait for all of them with "
                            "'await Promise.all(...)', or keep them in a variable");
    }
    return;
  }
  if (callback) {
    checker_.diags_.error(expr->loc(),
                          "the function returns a Promise that its caller drops, so nothing "
                          "waits for it to finish; loop with 'for (const x of xs) await "
                          "f(x)', or wait for all with 'await Promise.all(xs.map(...))'");
    return;
  }
  std::string text = statementText(at);
  if (expr->type && expr->type->isPromise() && Checker::holdsResults(expr->type->promised())) {
    checker_.diags_.error(expr->loc(),
                          "the Promise is not awaited, and the Result it gives would go "
                          "unchecked; await it and handle the Result, e.g. 'const r = await " +
                              (text.empty() ? std::string("p") : text) + "'");
  } else if (text.empty()) {
    checker_.diags_.error(expr->loc(),
                          "the Promise is not awaited, so nothing waits for it and an error in "
                          "it would end the program later; write 'await' before it, or "
                          "'void' to let it run on its own");
  } else {
    checker_.diags_.error(expr->loc(),
                          "the Promise is not awaited, so nothing waits for it and an error in "
                          "it would end the program later; write 'await " +
                              text + "', or 'void " + text + "' to let it run on its own");
  }
}

}
