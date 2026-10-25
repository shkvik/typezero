#include "compiler/check/stmt/loop_checker/loop_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/check/stmt/jump_checker/jump_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/loop_scanner/loop_scanner.h"
#include "compiler/check/flow/loop_bounds/loop_bounds.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/expr/truth_checker/truth_checker.h"
#include "compiler/library/regex/match_checker/match_checker.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "compiler/whole_program/flows/flows.h"
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

// Whether `stmt`, the body of a loop with `labels`, has a `continue` that
// goes to that loop; `loops` counts the loops around `stmt` inside it.
static bool continuesTo(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels,
                        unsigned loops = 0) {
  if (!stmt) {
    return false;
  }
  auto inside = [&](const Stmt *s) { return continuesTo(s, labels, loops); };
  switch (stmt->kind()) {
  case Stmt::Kind::Continue: {
    llvm::StringRef label = cast<ContinueStmt>(stmt)->label;
    return label.empty() ? loops == 0 : llvm::is_contained(labels, label);
  }
  case Stmt::Kind::Block:
    return llvm::any_of(cast<BlockStmt>(stmt)->body, inside);
  case Stmt::Kind::If:
    return inside(cast<IfStmt>(stmt)->thenStmt) || inside(cast<IfStmt>(stmt)->elseStmt);
  case Stmt::Kind::While:
    return continuesTo(cast<WhileStmt>(stmt)->body, labels, loops + 1);
  case Stmt::Kind::DoWhile:
    return continuesTo(cast<DoWhileStmt>(stmt)->body, labels, loops + 1);
  case Stmt::Kind::For:
    return continuesTo(cast<ForStmt>(stmt)->body, labels, loops + 1);
  case Stmt::Kind::ForOf:
    return continuesTo(cast<ForOfStmt>(stmt)->body, labels, loops + 1);
  case Stmt::Kind::Switch:
    return llvm::any_of(cast<SwitchStmt>(stmt)->cases,
                        [&](const SwitchCase &c) { return llvm::any_of(c.body, inside); });
  case Stmt::Kind::Labeled:
    return inside(cast<LabeledStmt>(stmt)->body);
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    return inside(s->block) || inside(s->handler) || inside(s->finalizer);
  }
  default:
    return false;
  }
}

LoopChecker::LoopChecker(Sema &sema) : sema_(sema) {}

// Loops are checked once. Their head state comes from scanning the loop for
// what it changes (loopHead), so what Sema assumes at the start of the body
// holds on every iteration, and everywhere in the loop.
void LoopChecker::checkWhile(WhileStmt *stmt) {
  Sema::LoopScan scan = sema_.loopScanner().scanLoop({stmt->body}, {stmt->cond});
  Sema::Flow head = sema_.loopBounds().loopHead(scan, stmt->cond, /*induction=*/true);
  sema_.flow_ = head;
  Sema::Flow whenTrue, whenFalse;
  stmt->cond = sema_.truthChecker().checkCondition(stmt->cond, whenTrue, whenFalse);
  sema_.flow_ = whenTrue;
  checkLoopBody(stmt->body);
  sema_.flow_ = scan.breaks ? FlowChecker::join(whenFalse, head) : whenFalse;
}

void LoopChecker::checkDoWhile(DoWhileStmt *stmt) {
  // No induction: the body runs once before the condition is first checked.
  Sema::LoopScan scan = sema_.loopScanner().scanLoop({stmt->body}, {stmt->cond});
  Sema::Flow head = sema_.loopBounds().loopHead(scan, nullptr, /*induction=*/false);
  sema_.flow_ = head;
  checkLoopBody(stmt->body);
  sema_.flow_ = FlowChecker::join(sema_.flow_, head); // `continue` jumps to the condition
  Sema::Flow whenTrue, whenFalse;
  stmt->cond = sema_.truthChecker().checkCondition(stmt->cond, whenTrue, whenFalse);
  sema_.flow_ = scan.breaks ? FlowChecker::join(whenFalse, head) : whenFalse;
}

void LoopChecker::checkFor(ForStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels = sema_.pendingLabels_;
  sema_.pushScope(); // variables declared in the loop header
  sema_.block_ = {};
  if (stmt->init) {
    sema_.stmtChecker().checkStmt(stmt->init);
  }
  Sema::LoopScan scan = sema_.loopScanner().scanLoop({stmt->body}, {stmt->cond, stmt->step});
  Sema::Flow head = sema_.loopBounds().loopHead(scan, stmt->cond, /*induction=*/true);
  sema_.flow_ = head;
  Sema::Flow whenTrue = head, whenFalse = head;
  whenFalse.unreachable = true;
  if (stmt->cond) {
    stmt->cond = sema_.truthChecker().checkCondition(stmt->cond, whenTrue, whenFalse);
  }
  sema_.flow_ = whenTrue;
  checkLoopBody(stmt->body);
  if (stmt->step) {
    // The step runs where the body ends (`node = node.next` after
    // `node != undefined`), unless a `continue` may jump there from
    // elsewhere; what holds at the head holds there too. If the body never
    // ends, the step never runs, and is checked as the body starts.
    if (continuesTo(stmt->body, labels)) {
      sema_.flow_ = head;
    } else if (sema_.flow_.unreachable) {
      sema_.flow_ = whenTrue;
    }
    if (auto *yield = dyn_cast<YieldExpr>(stmt->step)) {
      yield->discarded = true; // as in a statement
    }
    stmt->step = sema_.exprChecker().checkExpr(stmt->step);
    sema_.resultChecker().checkDiscarded(stmt->step);
  }
  sema_.flow_ = scan.breaks ? FlowChecker::join(whenFalse, head) : whenFalse;
  sema_.popScope();
}

void LoopChecker::checkForOf(ForOfStmt *stmt) {
  if (stmt->isAwait) {
    sema_.asyncChecker().checkForAwait(stmt);
    return;
  }
  if (stmt->keys) {
    sema_.objectChecker().checkForIn(stmt);
  } else {
    sema_.iteratorMethods().markFused(stmt->iterable);
    stmt->iterable = sema_.dictChecker().iterableOf(sema_.genericChecker().apparent(sema_.exprChecker().checkExpr(stmt->iterable)));
    // `for (const x of args)` of an argument list (see expandArgumentList):
    // its elements, of the type that holds them all. (Any other tuple is not
    // iterated: its elements have types of their own.)
    auto *name = dyn_cast<NameExpr>(stmt->iterable);
    bool list = name && name->var && name->type->isTuple() && !name->type->tupleRest() &&
                llvm::any_of(sema_.functions_, [&](const FunctionDecl *fn) {
                  return fn->listName == name->name && name->var->owner == fn;
                });
    if (list) {
      auto *elements = sema_.ctx_.make<ArrayLitExpr>(name->loc());
      // The numbers as the widest of them, then a union of the kinds.
      const Type *number = nullptr;
      std::vector<const Type *> kinds;
      for (const Field &f : name->type->fields()) {
        if (f.type->isNumeric()) {
          number = number ? TypeRelations::commonNumericType(number, f.type) : f.type;
        }
      }
      for (const Field &f : name->type->fields()) {
        kinds.push_back(f.type->isNumeric() ? number : f.type);
      }
      for (size_t k = 0; k < name->type->fields().size(); ++k) {
        elements->elements.push_back(
            {sema_.ctx_.make<IndexExpr>(name->loc(), sema_.ctx_.make<NameExpr>(name->loc(), name->name),
                                  sema_.ctx_.make<IntLitExpr>(name->loc(), k)),
             /*spread=*/false});
      }
      const Type *element = kinds.empty() ? nullptr : Type::unionOf(kinds);
      if (!elements->elements.empty()) {
        stmt->iterable = sema_.exprChecker().checkExpr(
            elements, element && unionProblem(element).empty() ? Type::array(element) : nullptr);
      }
    }
  }
  if (stmt->iterable->type->isMatch()) { // the captures
    stmt->iterable = sema_.matchChecker().matchItems(stmt->iterable);
  }
  const Type *type = stmt->iterable->type;
  const Type *element = Type::error();
  if (type->isArray()) {
    element = type->inner();
  } else if (type->isString()) {
    element = Type::string();
  } else if (type->isTypedArray()) {
    element = type->elementType();
  } else if (type->isIterator()) {
    element = type->yielded();
    sema_.generatorChecker().checkSendsNothing(type, stmt->iterable->loc(), "'for...of'");
  } else if (type->isOptional() && GeneratorChecker::iteratedType(type->inner())) {
    sema_.diags_.error(stmt->iterable->loc(),
                 "'" + type->name() + "' may be undefined; check it before iterating");
  } else if (type->isTuple()) {
    sema_.diags_.error(stmt->iterable->loc(),
                 "cannot iterate over a tuple, whose elements have types of their own; "
                 "destructure it, or copy it into an array: '[...t]'");
  } else if (type->isRecord()) {
    sema_.diags_.error(stmt->iterable->loc(),
                 "a Record is not iterable, as an object is not in JavaScript; walk its keys "
                 "with 'for...in', or its entries with 'for (const [k, v] of Object.entries(r))'");
  } else if (type->isRuntimeObject() && sema_.generatorChecker().asyncIteratedType(type)) {
    sema_.diags_.error(stmt->iterable->loc(), "'" + type->name() +
                                            "' gives its values later: iterate it with 'for await "
                                            "(const x of ...)'");
  } else if (!type->isError()) {
    sema_.diags_.error(stmt->iterable->loc(),
                 "cannot iterate over a value of type '" + type->name() + "'");
  }
  if (stmt->binding.typeAnn) {
    sema_.diags_.error(stmt->binding.typeAnn->loc,
                 stmt->keys ? "a 'for...in' variable cannot have a type annotation"
                            : "a 'for...of' variable cannot have a type annotation");
  }
  if (stmt->binding.destructured && !stmt->binding.objectPattern && !element->isArray() &&
      !element->isTuple() && !element->isMatch() && !element->isError()) {
    sema_.diags_.error(stmt->binding.loc,
                 "only arrays can be destructured; the elements are '" + element->name() + "'");
    element = Type::error();
  }

  Sema::LoopScan scan = sema_.loopScanner().scanLoop({stmt->body}, {});
  // An iterator runs code between the iterations: a generator's body, the
  // callbacks of its helpers.
  if (type->isIterator()) {
    scan.callsFunctions = true;
  }
  // The array keeps each element alive during its iteration unless the body
  // could remove or replace elements, directly or through a function. Then a
  // const loop variable can borrow the element instead of counting it.
  stmt->borrowElements = stmt->isConst && type->isArray() && element->isManaged() &&
                         !scan.callsFunctions && !scan.releasesElements;
  Sema::Flow head = sema_.loopBounds().loopHead(scan, nullptr, /*induction=*/false);
  sema_.flow_ = head;
  sema_.pushScope();
  stmt->binding.sym = sema_.scopes().declareVar(stmt->binding.name, stmt->binding.loc, element, stmt->isConst);
  checkLoopBody(stmt->body);
  sema_.popScope();
  sema_.flow_ = head;
}

void LoopChecker::checkLoopBody(Stmt *body) {
  sema_.jumpChecker().pushTarget(Sema::JumpTarget::Kind::Loop);
  sema_.stmtChecker().checkBody(body);
  sema_.targets_.pop_back();
}

} // namespace tz
