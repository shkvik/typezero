#include "compiler/check/stmt/loop_checker/loop_checker.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/truth_checker/truth_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/loop_bounds/loop_bounds.h"
#include "compiler/check/flow/loop_scanner/loop_scanner.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/stmt/jump_checker/jump_checker.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
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
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

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

LoopChecker::LoopChecker(Checker &checker) : checker_(checker) {}

void LoopChecker::checkWhile(WhileStmt *stmt) {
  Checker::LoopScan scan = checker_.loopScanner().scanLoop({stmt->body}, {stmt->cond});
  Checker::Flow head = checker_.loopBounds().loopHead(scan, stmt->cond, true);
  checker_.flow_ = head;
  Checker::Flow whenTrue, whenFalse;
  stmt->cond = checker_.truthChecker().checkCondition(stmt->cond, whenTrue, whenFalse);
  checker_.flow_ = whenTrue;
  checkLoopBody(stmt->body);
  checker_.flow_ = scan.breaks ? FlowChecker::join(whenFalse, head) : whenFalse;
}

void LoopChecker::checkDoWhile(DoWhileStmt *stmt) {
  Checker::LoopScan scan = checker_.loopScanner().scanLoop({stmt->body}, {stmt->cond});
  Checker::Flow head = checker_.loopBounds().loopHead(scan, nullptr, false);
  checker_.flow_ = head;
  checkLoopBody(stmt->body);
  checker_.flow_ = FlowChecker::join(checker_.flow_, head);
  Checker::Flow whenTrue, whenFalse;
  stmt->cond = checker_.truthChecker().checkCondition(stmt->cond, whenTrue, whenFalse);
  checker_.flow_ = scan.breaks ? FlowChecker::join(whenFalse, head) : whenFalse;
}

void LoopChecker::checkFor(ForStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels = checker_.pendingLabels_;
  checker_.pushScope();
  checker_.block_ = {};
  if (stmt->init) {
    checker_.stmtChecker().checkStmt(stmt->init);
  }
  Checker::LoopScan scan = checker_.loopScanner().scanLoop({stmt->body}, {stmt->cond, stmt->step});
  Checker::Flow head = checker_.loopBounds().loopHead(scan, stmt->cond, true);
  checker_.flow_ = head;
  Checker::Flow whenTrue = head, whenFalse = head;
  whenFalse.unreachable = true;
  if (stmt->cond) {
    stmt->cond = checker_.truthChecker().checkCondition(stmt->cond, whenTrue, whenFalse);
  }
  checker_.flow_ = whenTrue;
  checkLoopBody(stmt->body);
  if (stmt->step) {
    if (continuesTo(stmt->body, labels)) {
      checker_.flow_ = head;
    } else if (checker_.flow_.unreachable) {
      checker_.flow_ = whenTrue;
    }
    if (auto *yield = dyn_cast<YieldExpr>(stmt->step)) {
      yield->discarded = true;
    }
    stmt->step = checker_.exprChecker().checkExpr(stmt->step);
    checker_.resultChecker().checkDiscarded(stmt->step);
  }
  checker_.flow_ = scan.breaks ? FlowChecker::join(whenFalse, head) : whenFalse;
  checker_.popScope();
}

void LoopChecker::checkForOf(ForOfStmt *stmt) {
  if (stmt->isAwait) {
    checker_.asyncChecker().checkForAwait(stmt);
    return;
  }
  if (stmt->keys) {
    checker_.objectChecker().checkForIn(stmt);
  } else {
    checker_.iteratorMethods().markFused(stmt->iterable);
    stmt->iterable = checker_.dictChecker().iterableOf(
        checker_.genericChecker().apparent(checker_.exprChecker().checkExpr(stmt->iterable)));
    auto *name = dyn_cast<NameExpr>(stmt->iterable);
    bool list = name && name->var && name->type->isTuple() && !name->type->tupleRest() &&
                llvm::any_of(checker_.functions_, [&](const FunctionDecl *fn) {
                  return fn->listName == name->name && name->var->owner == fn;
                });
    if (list) {
      auto *elements = checker_.ctx_.make<ArrayLitExpr>(name->loc());
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
            {checker_.ctx_.make<IndexExpr>(name->loc(),
                                           checker_.ctx_.make<NameExpr>(name->loc(), name->name),
                                           checker_.ctx_.make<IntLitExpr>(name->loc(), k)),
             false});
      }
      const Type *element = kinds.empty() ? nullptr : Type::unionOf(kinds);
      if (!elements->elements.empty()) {
        stmt->iterable = checker_.exprChecker().checkExpr(
            elements, element && unionProblem(element).empty() ? Type::array(element) : nullptr);
      }
    }
  }
  if (stmt->iterable->type->isMatch()) {
    stmt->iterable = checker_.matchChecker().matchItems(stmt->iterable);
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
    checker_.generatorChecker().checkSendsNothing(type, stmt->iterable->loc(), "'for...of'");
  } else if (type->isOptional() && GeneratorChecker::iteratedType(type->inner())) {
    checker_.diags_.error(stmt->iterable->loc(),
                          "'" + type->name() + "' may be undefined; check it before iterating");
  } else if (type->isTuple()) {
    checker_.diags_.error(stmt->iterable->loc(),
                          "cannot iterate over a tuple, whose elements have types of their own; "
                          "destructure it, or copy it into an array: '[...t]'");
  } else if (type->isRecord()) {
    checker_.diags_.error(
        stmt->iterable->loc(),
        "a Record is not iterable, as an object is not in JavaScript; walk its keys "
        "with 'for...in', or its entries with 'for (const [k, v] of Object.entries(r))'");
  } else if (type->isRuntimeObject() && checker_.generatorChecker().asyncIteratedType(type)) {
    checker_.diags_.error(stmt->iterable->loc(),
                          "'" + type->name() +
                              "' gives its values later: iterate it with 'for await "
                              "(const x of ...)'");
  } else if (!type->isError()) {
    checker_.diags_.error(stmt->iterable->loc(),
                          "cannot iterate over a value of type '" + type->name() + "'");
  }
  if (stmt->binding.typeAnn) {
    checker_.diags_.error(stmt->binding.typeAnn->loc,
                          stmt->keys ? "a 'for...in' variable cannot have a type annotation"
                                     : "a 'for...of' variable cannot have a type annotation");
  }
  if (stmt->binding.destructured && !stmt->binding.objectPattern && !element->isArray() &&
      !element->isTuple() && !element->isMatch() && !element->isError()) {
    checker_.diags_.error(stmt->binding.loc, "only arrays can be destructured; the elements are '" +
                                                 element->name() + "'");
    element = Type::error();
  }

  Checker::LoopScan scan = checker_.loopScanner().scanLoop({stmt->body}, {});
  if (type->isIterator()) {
    scan.callsFunctions = true;
  }
  stmt->borrowElements = stmt->isConst && type->isArray() && element->isManaged() &&
                         !scan.callsFunctions && !scan.releasesElements;
  Checker::Flow head = checker_.loopBounds().loopHead(scan, nullptr, false);
  checker_.flow_ = head;
  checker_.pushScope();
  stmt->binding.sym =
      checker_.scopes().declareVar(stmt->binding.name, stmt->binding.loc, element, stmt->isConst);
  checkLoopBody(stmt->body);
  checker_.popScope();
  checker_.flow_ = head;
}

void LoopChecker::checkLoopBody(Stmt *body) {
  checker_.jumpChecker().pushTarget(Checker::JumpTarget::Kind::Loop);
  checker_.stmtChecker().checkBody(body);
  checker_.targets_.pop_back();
}

}
