#include "compiler/check/stmt/return_checker/return_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/types/destination/destination.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/condition_facts/condition_facts.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/flow/predicate_checker/predicate_checker.h"
#include "compiler/check/expr/truth_checker/truth_checker.h"
#include "compiler/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

static bool isLoop(const Stmt *stmt) {
  return isa<WhileStmt>(stmt) || isa<DoWhileStmt>(stmt) || isa<ForStmt>(stmt) ||
         isa<ForOfStmt>(stmt);
}

static bool isTrueLiteral(const Expr *expr) {
  auto *lit = llvm::dyn_cast_or_null<BoolLitExpr>(expr);

  return lit && lit->value;
}

namespace {

// Finds the `break` and `continue` statements inside a statement S that
// leave S: those that jump to S itself (except `continue` to a loop S) or to
// a statement around it.
class Exits {
public:
  Exits(bool isLoop, llvm::ArrayRef<llvm::StringRef> labels) : isLoop_(isLoop), labels_(labels) {}

  bool any(llvm::ArrayRef<Stmt *> stmts) {
    return llvm::any_of(stmts, [&](const Stmt *s) { return in(s); });
  }

  bool in(const Stmt *stmt) {
    switch (stmt->kind()) {
    case Stmt::Kind::Break: {
      llvm::StringRef label = cast<BreakStmt>(stmt)->label;
      return label.empty() ? breakables_ == 0 : !llvm::is_contained(inner_, label);
    }
    case Stmt::Kind::Continue: {
      llvm::StringRef label = cast<ContinueStmt>(stmt)->label;
      if (label.empty()) {
        return loops_ == 0 && !isLoop_;
      }
      if (llvm::is_contained(inner_, label)) {
        return false;
      }
      return !(isLoop_ && llvm::is_contained(labels_, label));
    }
    case Stmt::Kind::Block:
      return any(cast<BlockStmt>(stmt)->body);
    case Stmt::Kind::If: {
      auto *s = cast<IfStmt>(stmt);
      return in(s->thenStmt) || (s->elseStmt && in(s->elseStmt));
    }
    case Stmt::Kind::While:
      return inLoop(cast<WhileStmt>(stmt)->body);
    case Stmt::Kind::DoWhile:
      return inLoop(cast<DoWhileStmt>(stmt)->body);
    case Stmt::Kind::For:
      return inLoop(cast<ForStmt>(stmt)->body);
    case Stmt::Kind::ForOf:
      return inLoop(cast<ForOfStmt>(stmt)->body);
    case Stmt::Kind::Switch: {
      ++breakables_;
      bool found = llvm::any_of(cast<SwitchStmt>(stmt)->cases,
                                [&](const SwitchCase &c) { return any(c.body); });
      --breakables_;
      return found;
    }
    case Stmt::Kind::Labeled: {
      auto *s = cast<LabeledStmt>(stmt);
      inner_.push_back(s->label);
      bool found = in(s->body);
      inner_.pop_back();
      return found;
    }
    case Stmt::Kind::Try: {
      auto *s = cast<TryStmt>(stmt);
      return in(s->block) || (s->handler && in(s->handler)) || (s->finalizer && in(s->finalizer));
    }
    default:
      return false;
    }
  }

private:
  bool inLoop(const Stmt *body) {
    ++loops_;
    ++breakables_;
    bool found = in(body);
    --loops_;
    --breakables_;

    return found;
  }

  bool isLoop_;
  llvm::ArrayRef<llvm::StringRef> labels_;      // of S
  llvm::SmallVector<llvm::StringRef, 4> inner_; // declared inside S
  unsigned loops_ = 0, breakables_ = 0;         // inside S, around the current statement
};

// Whether some `break` inside `stmt` jumps to one of `labels`.
bool breaksTo(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels) {
  if (labels.empty()) {
    return false;
  }
  switch (stmt->kind()) {
  case Stmt::Kind::Break:
    return llvm::is_contained(labels, cast<BreakStmt>(stmt)->label);
  case Stmt::Kind::Block:
    return llvm::any_of(cast<BlockStmt>(stmt)->body,
                        [&](const Stmt *s) { return breaksTo(s, labels); });
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    return breaksTo(s->thenStmt, labels) || (s->elseStmt && breaksTo(s->elseStmt, labels));
  }
  case Stmt::Kind::While:
    return breaksTo(cast<WhileStmt>(stmt)->body, labels);
  case Stmt::Kind::DoWhile:
    return breaksTo(cast<DoWhileStmt>(stmt)->body, labels);
  case Stmt::Kind::For:
    return breaksTo(cast<ForStmt>(stmt)->body, labels);
  case Stmt::Kind::ForOf:
    return breaksTo(cast<ForOfStmt>(stmt)->body, labels);
  case Stmt::Kind::Switch:
    return llvm::any_of(cast<SwitchStmt>(stmt)->cases, [&](const SwitchCase &c) {
      return llvm::any_of(c.body, [&](const Stmt *s) { return breaksTo(s, labels); });
    });
  case Stmt::Kind::Labeled:
    return breaksTo(cast<LabeledStmt>(stmt)->body, labels);
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    return breaksTo(s->block, labels) || (s->handler && breaksTo(s->handler, labels)) ||
           (s->finalizer && breaksTo(s->finalizer, labels));
  }
  default:
    return false;
  }
}

} // namespace

ReturnChecker::ReturnChecker(Sema &sema) : sema_(sema) {}

// The value of a return: of a function that returns a boolean, checked as a
// condition, so that the function's summary knows what it shows (see
// sema_predicate.cpp).
Expr *ReturnChecker::checkReturnValue(ReturnStmt *ret, const Type *hint, bool tested) {
  if (sema_.predicateBuilds_.empty() || !sema_.predicateBuilds_.back().fn) {
    return sema_.exprChecker().checkExpr(ret->value, hint);
  }
  if (!tested) {
    // A boolean all the same, such as what a function that tests returns:
    // what it says as a condition.
    Expr *value = sema_.exprChecker().checkExpr(ret->value, hint);
    if (value->type->isBool()) {
      Sema::Flow whenTrue = sema_.flow_, whenFalse = sema_.flow_;
      sema_.conditionFacts().addConditionFacts(value, whenTrue, whenFalse);
      sema_.predicateChecker().notePredicateExit(whenTrue, whenFalse, ret->value->loc());
    }
    return value;
  }
  Sema::Flow whenTrue, whenFalse;
  Expr *value = sema_.truthChecker().checkBranching(ret->value, whenTrue, whenFalse);
  sema_.flow_ = FlowChecker::join(whenTrue, whenFalse);
  sema_.predicateChecker().notePredicateExit(whenTrue, whenFalse, ret->value->loc());

  return value;
}

void ReturnChecker::checkReturn(ReturnStmt *ret) {
  if (!sema_.currentFunction_) {
    sema_.diags_.error(ret->loc(), "'return' can only be used inside a function");
    if (ret->value) {
      ret->value = sema_.exprChecker().checkExpr(ret->value);
    }
    return;
  }

  const FuncSymbol *fn = sema_.currentFunction_;
  if (fn->decl && fn->decl->isConstructor) {
    if (ret->value) {
      ret->value = sema_.exprChecker().checkExpr(ret->value);
      sema_.diags_.error(ret->value->loc(),
                   "a constructor cannot return a value: 'new' gives the object");
    } else if (sema_.initFn_ == fn->decl && sema_.flow_.beforeSuper) {
      sema_.diags_.error(ret->loc(), "the constructor returns before 'super(...)', which makes the "
                               "object (JavaScript throws a ReferenceError there)");
    } else if (sema_.initFn_ == fn->decl) {
      sema_.classChecker().checkFieldsSet(ret->loc());
    }
    return;
  }
  // What a generator returns is what its iterator gives when it is done; an
  // async function, what its Promise gives.
  const Type *returned = fn->ret && fn->decl && fn->decl->isGenerator &&
                                 (fn->ret->isIterator() || fn->ret->isAsyncIterator())
                             ? fn->ret->returned()
                         : fn->ret && fn->decl && fn->decl->isAsync && fn->ret->isPromise()
                             ? fn->ret->promised()
                             : fn->ret;
  if (!fn->ret) {
    // Its type comes from what its return statements give. A literal takes
    // the type of an object an earlier one gave.
    const Type *hint = nullptr;
    if (ret->value && Destination::literalShaped(ret->value)) {
      for (const ReturnStmt *earlier : *sema_.returns_) {
        if (earlier->value && !earlier->value->type->isError() &&
            (Sema::holdsObjects(earlier->value->type) || earlier->value->type->isResult())) {
          hint = earlier->value->type;
          break;
        }
      }
    }
    if (ret->value) {
      ret->value = checkReturnValue(ret, hint, PredicateChecker::looksBoolean(ret->value));
    }
    sema_.returns_->push_back(ret);
    return;
  }
  if (!ret->value) {
    if (fn->asserts) {
      sema_.predicateChecker().notePredicateExit(sema_.flow_, Sema::Flow(), ret->loc());
    }
    if (!returned->isVoid() && !returned->isError() && !returned->isOptional()) {
      sema_.diags_.error(ret->loc(), functionLabel(fn->name) + " must return a value of type '" +
                                   returned->name() + "'");
    }
    return;
  }

  // `return p` in an async generator awaits p.
  if (fn->decl && fn->decl->isAsync && fn->decl->isGenerator) {
    ret->value = sema_.exprChecker().checkExpr(ret->value, returned->isVoid() ? nullptr : returned);
    const Type *given = ret->value->type->isError() ? nullptr : AsyncChecker::awaitedType(ret->value->type);
    if (given && !given->isUndefined() && !given->isVoid() &&
        (returned->isVoid() || !TypeRelations::isImplicitlyConvertible(given, returned))) {
      sema_.diags_.error(ret->value->loc(), functionLabel(fn->name) + " returns '" + returned->name() +
                                          "', found '" + given->name() + "'");
    }
    return;
  }
  ret->value = checkReturnValue(ret, returned->isVoid() ? nullptr : returned, returned->isBool());
  // `return p` in an async function: its Promise follows p.
  if (fn->decl && fn->decl->isAsync && fn->ret->isPromise() && ret->value->type->isPromise()) {
    const Type *given = ret->value->type->promised();
    if (given != returned && !given->isNever()) {
      sema_.diags_.error(ret->value->loc(), "this Promise gives '" + given->name() + "', but " +
                                          functionLabel(fn->name) + " gives '" + returned->name() +
                                          "'; await it and convert the value");
    }
    return;
  }
  if (returned->isVoid()) {
    // `return f()` with a void `f` is fine, as in TypeScript, and so are
    // `return void f()` and `return undefined`, and any value in a callback
    // whose result is dropped.
    const Type *type = ret->value->type;
    if (!type->isVoid() && !type->isError() && !type->isUndefined() && !fn->discardsResult) {
      sema_.diags_.error(ret->value->loc(), functionLabel(fn->name) +
                                          " is declared to return 'void', "
                                          "but returns '" +
                                          type->name() + "'");
      sema_.classLinks().noteInheritedReturn(fn->decl);
    } else if (fn->discardsResult) {
      sema_.resultChecker().checkDiscarded(ret->value, /*callback=*/true);
    }
    return;
  }
  unsigned errors = sema_.diags_.errorCount();
  ret->value = sema_.typeRelations().coerce(ret->value, returned);
  if (sema_.diags_.errorCount() > errors) {
    sema_.classLinks().noteInheritedReturn(fn->decl);
  } else if (fn->decl) {
    sema_.byteProofs().checkByteCallbackResult(fn->decl, ret->value);
  }
}

bool ReturnChecker::isExit(const Stmt *stmt) {
  auto *s = dyn_cast<ExprStmt>(stmt);
  auto *call = s ? dyn_cast<CallExpr>(s->expr) : nullptr;

  return call && call->builtin == Builtin::ProcessExit;
}

// Conservative: true only if control can never reach the end of `stmt`.
bool ReturnChecker::alwaysReturns(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels) {
  auto returns = [](const Stmt *s) { return alwaysReturns(s); };
  switch (stmt->kind()) {
  case Stmt::Kind::Return:
  case Stmt::Kind::Throw:
    return true;
  case Stmt::Kind::Expr:
    return isExit(stmt);
  // A `finally` that never ends ends the whole statement; otherwise the
  // block, and the `catch` that runs when it throws, must both never end.
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    if (breaksTo(stmt, labels)) {
      return false;
    }
    if (s->finalizer && alwaysReturns(s->finalizer)) {
      return true;
    }
    return alwaysReturns(s->block) && (!s->handler || alwaysReturns(s->handler));
  }
  case Stmt::Kind::Block:
    return llvm::any_of(cast<BlockStmt>(stmt)->body, returns) && !breaksTo(stmt, labels);
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    // `if (true) S` runs S; so does the `if` of an instance whose types
    // decided the condition, which keeps only the branch that runs.
    if (auto *lit = dyn_cast<BoolLitExpr>(s->cond)) {
      if (lit->value && !s->elseStmt) {
        return alwaysReturns(s->thenStmt) && !breaksTo(stmt, labels);
      }
      if (!lit->value && isa<EmptyStmt>(s->thenStmt)) {
        return s->elseStmt && alwaysReturns(s->elseStmt) && !breaksTo(stmt, labels);
      }
    }
    return s->elseStmt && alwaysReturns(s->thenStmt) && alwaysReturns(s->elseStmt) &&
           !breaksTo(stmt, labels);
  }
  // An endless loop never falls through unless something leaves it.
  case Stmt::Kind::While: {
    auto *s = cast<WhileStmt>(stmt);
    return isTrueLiteral(s->cond) && !Exits(true, labels).in(s->body);
  }
  case Stmt::Kind::DoWhile: {
    auto *s = cast<DoWhileStmt>(stmt);
    return isTrueLiteral(s->cond) && !Exits(true, labels).in(s->body);
  }
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    return (!s->cond || isTrueLiteral(s->cond)) && !Exits(true, labels).in(s->body);
  }
  // A switch with `default` always enters some case, and control falls
  // through to the end of the last one unless something leaves the switch.
  case Stmt::Kind::Switch: {
    auto *s = cast<SwitchStmt>(stmt);
    bool hasDefault =
        s->exhaustive || llvm::any_of(s->cases, [](const SwitchCase &c) { return !c.test; });
    if (!hasDefault || s->cases.empty()) {
      return false;
    }
    Exits exits(false, labels);
    if (llvm::any_of(s->cases, [&](const SwitchCase &c) { return exits.any(c.body); })) {
      return false;
    }
    return llvm::any_of(s->cases.back().body, returns);
  }
  case Stmt::Kind::Labeled: {
    llvm::SmallVector<llvm::StringRef, 2> all(labels.begin(), labels.end());
    while (auto *labeled = dyn_cast<LabeledStmt>(stmt)) {
      all.push_back(labeled->label);
      stmt = labeled->body;
    }
    return alwaysReturns(stmt, all);
  }
  default:
    return false;
  }
}

} // namespace tz
