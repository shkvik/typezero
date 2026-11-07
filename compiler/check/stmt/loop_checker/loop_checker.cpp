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
#include "compiler/check/objects/tuple_checker/tuple_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/stmt/jump_checker/jump_checker.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/regex/match_checker/match_checker.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;

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
  default:
    return false;
  }
}

LoopChecker::LoopChecker(Checker &owner)
    : checker(owner) {}

void LoopChecker::checkWhile(WhileStmt *stmt) {
  Checker::LoopScan scan = this->checker.loopScanner().scanLoop({stmt->body}, {stmt->cond});
  Checker::Flow head = this->checker.loopBounds().loopHead(scan, stmt->cond, true);
  this->checker.flow_ = head;
  Checker::Flow whenTrue, whenFalse;
  stmt->cond = this->checker.truthChecker().checkCondition(stmt->cond, whenTrue, whenFalse);
  this->checker.flow_ = whenTrue;
  this->checkLoopBody(stmt->body, scan.awaits);
  this->checker.flow_ = scan.breaks ? FlowChecker::join(whenFalse, head) : whenFalse;
}

void LoopChecker::checkDoWhile(DoWhileStmt *stmt) {
  Checker::LoopScan scan = this->checker.loopScanner().scanLoop({stmt->body}, {stmt->cond});
  Checker::Flow head = this->checker.loopBounds().loopHead(scan, nullptr, false);
  this->checker.flow_ = head;
  this->checkLoopBody(stmt->body, scan.awaits);
  this->checker.flow_ = FlowChecker::join(this->checker.flow_, head);
  Checker::Flow whenTrue, whenFalse;
  stmt->cond = this->checker.truthChecker().checkCondition(stmt->cond, whenTrue, whenFalse);
  this->checker.flow_ = scan.breaks ? FlowChecker::join(whenFalse, head) : whenFalse;
}

void LoopChecker::checkFor(ForStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels = this->checker.pendingLabels_;
  this->checker.pushScope();
  this->checker.block_ = {};
  if (stmt->init) {
    this->checker.stmtChecker().checkStmt(stmt->init);
  }
  Checker::LoopScan scan =
      this->checker.loopScanner().scanLoop({stmt->body}, {stmt->cond, stmt->step});
  Checker::Flow head = this->checker.loopBounds().loopHead(scan, stmt->cond, true);
  this->checker.flow_ = head;
  Checker::Flow whenTrue = head;
  Checker::Flow whenFalse = head;
  whenFalse.unreachable = true;
  if (stmt->cond) {
    stmt->cond = this->checker.truthChecker().checkCondition(stmt->cond, whenTrue, whenFalse);
  }
  this->checker.flow_ = whenTrue;
  this->checkLoopBody(stmt->body, scan.awaits);
  if (stmt->step) {
    if (continuesTo(stmt->body, labels)) {
      this->checker.flow_ = head;
    } else if (this->checker.flow_.unreachable) {
      this->checker.flow_ = whenTrue;
    }
    if (auto *yield = dyn_cast<YieldExpr>(stmt->step)) {
      yield->discarded = true;
    }
    stmt->step = this->checker.exprChecker().checkExpr(stmt->step);
    this->checker.resultChecker().checkDiscarded(stmt->step);
  }
  this->checker.flow_ = scan.breaks ? FlowChecker::join(whenFalse, head) : whenFalse;
  this->checker.popScope();
}

void LoopChecker::checkForOf(ForOfStmt *stmt) {
  if (stmt->isAwait) {
    this->checker.asyncChecker().checkForAwait(stmt);
    this->checker.emitterChecker().noteErrorsHandled(stmt->iterable);
    return;
  }
  if (stmt->keys) {
    this->checker.objectChecker().checkForIn(stmt);
  } else {
    this->checker.iteratorMethods().markFused(stmt->iterable);
    stmt->iterable = this->checker.dictChecker().iterableOf(this->checker.genericChecker().apparent(
        this->checker.exprChecker().checkExpr(stmt->iterable)));
    auto *name = dyn_cast<NameExpr>(stmt->iterable);
    bool list = name && name->var() && name->type->isTuple() && !name->type->tupleRest() &&
                llvm::any_of(this->checker.functions_, [&](const FunctionDecl *fn) {
                  return fn->listName == name->name && name->var()->owner() == fn;
                });
    if (list) {
      auto *elements = this->checker.ctx_.make<ArrayLitExpr>(name->loc());
      std::vector<const Type *> kinds;
      for (const Field &f : name->type->fields()) {
        kinds.push_back(f.type);
      }
      for (size_t k = 0; k < name->type->fields().size(); ++k) {
        elements->elements.push_back(
            {this->checker.ctx_.make<IndexExpr>(
                 name->loc(), this->checker.ctx_.make<NameExpr>(name->loc(), name->name),
                 this->checker.ctx_.make<IntLitExpr>(name->loc(), k)),
             false});
      }
      const Type *element = kinds.empty() ? nullptr : Type::unionOf(kinds);
      if (!elements->elements.empty()) {
        stmt->iterable = this->checker.exprChecker().checkExpr(
            elements, element && unionProblem(element).empty() ? Type::array(element) : nullptr);
      }
    }
  }
  if (stmt->iterable->type->isMatch()) {
    stmt->iterable = this->checker.matchChecker().matchItems(stmt->iterable);
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
    GeneratorChecker::ResultNextNeedsNoValue nothing =
        this->checker.generatorChecker().checkSendsNothing(type, "'for...of'");
    if (nothing.isErr()) {
      this->checker.diags_.error(stmt->iterable->loc(), nothing.error().message);
    }
  } else if (Expr *copy = this->checker.tupleChecker().arrayCopy(stmt->iterable)) {
    stmt->iterable = copy;
    element = copy->type->inner();
  } else {
    ResultIterableType iterable = this->iterableType(type);
    if (iterable.isErr()) {
      this->checker.diags_.error(stmt->iterable->loc(), iterable.error().message);
    }
  }
  ResultBindingTyped typed = LoopChecker::bindingType(stmt);
  if (typed.isErr()) {
    this->checker.diags_.error(stmt->binding.typeAnn->loc, typed.error().message);
  }
  ResultBindingShape shape = LoopChecker::bindingShape(stmt, element);
  if (shape.isErr()) {
    this->checker.diags_.error(stmt->binding.loc, shape.error().message);
    element = Type::error();
  }

  Checker::LoopScan scan = this->checker.loopScanner().scanLoop({stmt->body}, {});
  if (type->isIterator()) {
    scan.callsFunctions = true;
  }
  stmt->borrowElements = stmt->isConst && type->isArray() && element->isManaged() &&
                         !scan.callsFunctions && !scan.releasesElements;
  Checker::Flow head = this->checker.loopBounds().loopHead(scan, nullptr, false);
  this->checker.flow_ = head;
  this->checker.pushScope();
  stmt->binding.sym = this->checker.scopes().declareVar(stmt->binding.name, stmt->binding.loc,
                                                        element, stmt->isConst);
  if (auto *lit = dyn_cast<ArrayLitExpr>(stmt->iterable);
      lit && element->isInt() && stmt->binding.sym && !stmt->binding.destructured() &&
      !lit->elements.empty()) {
    std::optional<int64_t> lo, hi;
    for (const ArrayLitExpr::Element &e : lit->elements) {
      std::optional<int64_t> c =
          e.spread || e.hole ? std::nullopt : this->checker.constantFolder().intConstant(e.expr);
      if (!c) {
        lo.reset();
        break;
      }
      lo = lo ? std::min(*lo, *c) : *c;
      hi = hi ? std::max(*hi, *c) : *c;
    }
    if (lo) {
      this->checker.flow_.facts.add(Term::zero(), Term::of(stmt->binding.sym),
                                    saturatingSub(1, *lo));
      this->checker.flow_.facts.add(Term::of(stmt->binding.sym), Term::zero(),
                                    saturatingAdd(*hi, 1));
    }
  }
  this->checkLoopBody(stmt->body, scan.awaits);
  this->checker.popScope();
  this->checker.flow_ = head;
}

LoopChecker::ResultIterableType LoopChecker::iterableType(const Type *type) const {
  if (type->isOptional() && GeneratorChecker::iteratedType(type->inner())) {
    std::string message = ErrorTextForOf::mayBeUndef(type->name());

    return err(LoopCheckerIterableTypeError{
        .code = LoopCheckerIterableTypeErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type->isTuple()) {
    std::string message = ErrorTextForOf::tuple();

    return err(LoopCheckerIterableTypeError{
        .code = LoopCheckerIterableTypeErrorCode::TUPLE_NOT_ITERABLE,
        .message = message,
    });
  }
  if (type->isRecord()) {
    std::string message = ErrorTextForOf::record();

    return err(LoopCheckerIterableTypeError{
        .code = LoopCheckerIterableTypeErrorCode::RECORD_NOT_ITERABLE,
        .message = message,
    });
  }
  if (type->isRuntimeObject() && this->checker.generatorChecker().asyncIteratedType(type)) {
    std::string message = ErrorTextForOf::later(type->name());

    return err(LoopCheckerIterableTypeError{
        .code = LoopCheckerIterableTypeErrorCode::ASYNC_ONLY,
        .message = message,
    });
  }
  const ClassDecl *cls = type->classDecl();
  if (cls && cls->library() && cls->generic() && cls->generic()->name == "ConcurrentMap") {
    std::string message = ErrorTextForOf::concurrent();

    return err(LoopCheckerIterableTypeError{
        .code = LoopCheckerIterableTypeErrorCode::CONCURRENT_MAP,
        .message = message,
    });
  }
  if (!type->isError()) {
    std::string message = ErrorTextForOf::cannot(type->name());

    return err(LoopCheckerIterableTypeError{
        .code = LoopCheckerIterableTypeErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(type);
}

LoopChecker::ResultBindingTyped LoopChecker::bindingType(const ForOfStmt *stmt) {
  if (stmt->binding.typeAnn && stmt->keys) {
    std::string message = ErrorTextLoopBinding::forIn();

    return err(LoopCheckerBindingTypeError{
        .code = LoopCheckerBindingTypeErrorCode::FOR_IN_ANNOTATED,
        .message = message,
    });
  }
  if (stmt->binding.typeAnn) {
    std::string message = ErrorTextLoopBinding::forOf();

    return err(LoopCheckerBindingTypeError{
        .code = LoopCheckerBindingTypeErrorCode::FOR_OF_ANNOTATED,
        .message = message,
    });
  }

  return ok(&stmt->binding);
}

LoopChecker::ResultBindingShape LoopChecker::bindingShape(const ForOfStmt *stmt,
                                                          const Type *element) {
  bool fits = !stmt->binding.destructured() || stmt->binding.objectPattern() ||
              element->isArray() || element->isTuple() || element->isMatch() || element->isError();
  if (!fits) {
    std::string message = ErrorTextLoopShape::notArray(element->name());

    return err(LoopCheckerBindingShapeError{
        .code = LoopCheckerBindingShapeErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(element);
}

void LoopChecker::checkLoopBody(Stmt *body, bool awaits) {
  this->checker.jumpChecker().pushTarget(Checker::JumpTarget::Kind::Loop);
  this->checker.awaitLoops_ += awaits;
  StmtChecker::ResultBodyIsStatement checked = this->checker.stmtChecker().checkBody(body);
  if (checked.isErr()) {
    this->checker.diags_.error(body->loc(), checked.error().message);
  }
  this->checker.awaitLoops_ -= awaits;
  this->checker.targets_.pop_back();
}

}
