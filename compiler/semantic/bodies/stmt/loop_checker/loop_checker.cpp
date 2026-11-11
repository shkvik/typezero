#include "compiler/semantic/bodies/stmt/loop_checker/loop_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/truth_checker/truth_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/semantic/bodies/objects/tuple_checker/tuple_checker.h"
#include "compiler/semantic/bodies/stmt/jump_checker/jump_checker.h"
#include "compiler/semantic/bodies/stmt/stmt_checker/stmt_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/loop_bounds/loop_bounds.h"
#include "compiler/semantic/flow/loop_scanner/loop_scanner.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/regex/match_checker/match_checker.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::dyn_cast;

namespace tz {

LoopChecker::LoopChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->loopBounds = &all.loopBounds;
  this->dictChecker = &all.dictChecker;
  this->exprChecker = &all.exprChecker;
  this->jumpChecker = &all.jumpChecker;
  this->loopScanner = &all.loopScanner;
  this->stmtChecker = &all.stmtChecker;
  this->asyncChecker = &all.asyncChecker;
  this->matchChecker = &all.matchChecker;
  this->truthChecker = &all.truthChecker;
  this->tupleChecker = &all.tupleChecker;
  this->objectChecker = &all.objectChecker;
  this->resultChecker = &all.resultChecker;
  this->constantFolder = &all.constantFolder;
  this->emitterChecker = &all.emitterChecker;
  this->genericChecker = &all.genericChecker;
  this->iteratorMethods = &all.iteratorMethods;
  this->generatorChecker = &all.generatorChecker;
}

bool LoopChecker::continuesTo(const Stmt *body, llvm::ArrayRef<llvm::StringRef> labels) {
  PlacedNodes nodes = PlacedNodes::collect({body}, {});
  for (const PlacedStmt &placed : nodes.getStmts()) {
    if (placed.place.isInsideFunction()) {
      continue;
    }
    auto *next = dyn_cast<ContinueStmt>(placed.stmt);
    if (next != nullptr && LoopChecker::continuesLoop(next, placed.place, labels)) {
      return true;
    }
  }

  return false;
}

bool LoopChecker::continuesLoop(const ContinueStmt *next, const TreePlace &place,
                                llvm::ArrayRef<llvm::StringRef> labels) {
  if (!next->label.empty()) {
    return llvm::is_contained(labels, next->label);
  }

  return !place.isInsideLoop();
}

void LoopChecker::walkWhile(WhileStmt *stmt) {
  LoopScan scan = this->loopScanner->scanLoop({stmt->body}, {stmt->cond});
  Flow head = this->loopBounds->loopHead(scan, stmt->cond, true);
  this->state.getFlowState().replaceFlow(head);
  Flow whenTrue, whenFalse;
  stmt->cond = this->truthChecker->checkCondition(stmt->cond, whenTrue, whenFalse);
  this->state.getFlowState().replaceFlow(whenTrue);
  this->walkLoopBody(stmt->body, scan.awaits);
  this->state.getFlowState().replaceFlow(scan.breaks ? FlowChecker::join(whenFalse, head)
                                                     : whenFalse);
}

void LoopChecker::walkDoWhile(DoWhileStmt *stmt) {
  LoopScan scan = this->loopScanner->scanLoop({stmt->body}, {stmt->cond});
  Flow head = this->loopBounds->loopHead(scan, nullptr, false);
  this->state.getFlowState().replaceFlow(head);
  this->walkLoopBody(stmt->body, scan.awaits);
  this->state.getFlowState().replaceFlow(
      FlowChecker::join(this->state.getFlowState().getFlow(), head));
  Flow whenTrue, whenFalse;
  stmt->cond = this->truthChecker->checkCondition(stmt->cond, whenTrue, whenFalse);
  this->state.getFlowState().replaceFlow(scan.breaks ? FlowChecker::join(whenFalse, head)
                                                     : whenFalse);
}

void LoopChecker::walkFor(ForStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels = this->state.getBlockPosition().getPendingLabels();
  this->state.getScopeStack().openScope();
  this->state.getBlockPosition().forgetBlock();
  if (stmt->init) {
    this->stmtChecker->walkStmt(stmt->init);
  }
  LoopScan scan = this->loopScanner->scanLoop({stmt->body}, {stmt->cond, stmt->step});
  Flow head = this->loopBounds->loopHead(scan, stmt->cond, true);
  this->state.getFlowState().replaceFlow(head);
  Flow whenTrue = head;
  Flow whenFalse = head;
  whenFalse.unreachable = true;
  if (stmt->cond) {
    stmt->cond = this->truthChecker->checkCondition(stmt->cond, whenTrue, whenFalse);
  }
  this->state.getFlowState().replaceFlow(whenTrue);
  this->walkLoopBody(stmt->body, scan.awaits);
  if (stmt->step) {
    if (LoopChecker::continuesTo(stmt->body, labels)) {
      this->state.getFlowState().replaceFlow(head);
    } else if (this->state.getFlowState().isUnreachable()) {
      this->state.getFlowState().replaceFlow(whenTrue);
    }
    if (auto *yield = dyn_cast<YieldExpr>(stmt->step)) {
      yield->discarded = true;
    }
    stmt->step = this->exprChecker->checkExpr(stmt->step);
    ResultChecker::ResultValueGetsUsed used = this->resultChecker->usedValue(stmt->step);
    if (used.isErr()) {
      this->state.getDiagnostics().error(stmt->step->loc(), used.error().message);
    }
  }
  this->state.getFlowState().replaceFlow(scan.breaks ? FlowChecker::join(whenFalse, head)
                                                     : whenFalse);
  this->scopes->closeScope();
}

void LoopChecker::walkForOf(ForOfStmt *stmt) {
  if (stmt->isAwait) {
    this->asyncChecker->walkForAwait(stmt);
    this->emitterChecker->noteErrorsHandled(stmt->iterable);
    return;
  }
  if (stmt->keys) {
    this->objectChecker->walkForIn(stmt);
  } else {
    this->walkIterable(stmt);
  }
  if (stmt->iterable->type->isMatch()) {
    stmt->iterable = this->matchChecker->matchItems(stmt->iterable);
  }
  const Type *type = stmt->iterable->type;
  const Type *element = this->iteratedElement(stmt);
  ResultBindingTyped typed = LoopChecker::bindingType(stmt);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(stmt->binding.typeAnn->loc, typed.error().message);
  }
  ResultBindingShape shape = LoopChecker::bindingShape(stmt, element);
  if (shape.isErr()) {
    this->state.getDiagnostics().error(stmt->binding.loc, shape.error().message);
    element = Type::error();
  }
  LoopScan scan = this->loopScanner->scanForOfLoop(stmt);
  if (type->isIterator()) {
    scan.callsFunctions = true;
  }
  stmt->borrowElements = stmt->isConst && type->isArray() && element->isManaged() &&
                         !scan.callsFunctions && !scan.releasesElements;
  Flow head = this->loopBounds->loopHead(scan, nullptr, false);
  this->state.getFlowState().replaceFlow(head);
  this->state.getScopeStack().openScope();
  stmt->binding.sym =
      this->scopes->declareVar(stmt->binding.name, stmt->binding.loc, element, stmt->isConst);
  this->addLiteralBounds(stmt, element);
  this->walkLoopBody(stmt->body, scan.awaits);
  this->scopes->closeScope();
  this->state.getFlowState().replaceFlow(head);
}

void LoopChecker::walkIterable(ForOfStmt *stmt) {
  this->iteratorMethods->markFused(stmt->iterable);
  Expr *checked = this->exprChecker->checkExpr(stmt->iterable);
  Expr *apparent = this->genericChecker->apparent(checked);
  stmt->iterable = this->dictChecker->iterableOf(apparent);
  auto *name = dyn_cast<NameExpr>(stmt->iterable);
  if (name == nullptr || !this->isListParam(name)) {
    return;
  }
  Expr *elements = this->listElements(name);
  if (elements != nullptr) {
    stmt->iterable = elements;
  }
}

bool LoopChecker::isListParam(const NameExpr *name) {
  bool isTuple = name->var() != nullptr && name->type->isTuple() && !name->type->tupleRest();
  if (!isTuple) {
    return false;
  }
  for (const FunctionDecl *fn : this->state.getFunctionStack().getFunctions()) {
    if (fn->listName == name->name && name->var()->owner() == fn) {
      return true;
    }
  }

  return false;
}

Expr *LoopChecker::listElements(const NameExpr *name) {
  llvm::SMLoc loc = name->loc();
  auto *elements = this->state.getAst().make<ArrayLitExpr>(loc);
  std::vector<const Type *> kinds;
  for (const Field &f : name->type->fields()) {
    kinds.push_back(f.type);
  }
  for (size_t k = 0; k < name->type->fields().size(); ++k) {
    auto *root = this->state.getAst().make<NameExpr>(loc, name->name);
    auto *position = this->state.getAst().make<IntLitExpr>(loc, k);
    auto *item = this->state.getAst().make<IndexExpr>(loc, root, position);
    elements->elements.push_back({item, false});
  }
  if (elements->elements.empty()) {
    return nullptr;
  }
  const Type *element = kinds.empty() ? nullptr : Type::unionOf(kinds);
  bool isTyped = element != nullptr && UnionSlots::unionProblem(element).empty();

  return this->exprChecker->checkExpr(elements, isTyped ? Type::array(element) : nullptr);
}

const Type *LoopChecker::iteratedElement(ForOfStmt *stmt) {
  const Type *type = stmt->iterable->type;
  if (type->isArray()) {
    return type->inner();
  }
  if (type->isString()) {
    return Type::string();
  }
  if (type->isTypedArray()) {
    return type->elementType();
  }
  if (type->isIterator()) {
    GeneratorChecker::ResultNextNeedsNoValue nothing =
        this->generatorChecker->checkSendsNothing(type, "'for...of'");
    if (nothing.isErr()) {
      this->state.getDiagnostics().error(stmt->iterable->loc(), nothing.error().message);
    }
    return type->yielded();
  }
  Expr *copy = this->tupleChecker->arrayCopy(stmt->iterable);
  if (copy != nullptr) {
    stmt->iterable = copy;
    return copy->type->inner();
  }
  ResultIterableType iterable = this->iterableType(type);
  if (iterable.isErr()) {
    this->state.getDiagnostics().error(stmt->iterable->loc(), iterable.error().message);
  }

  return Type::error();
}

void LoopChecker::addLiteralBounds(const ForOfStmt *stmt, const Type *element) {
  auto *lit = dyn_cast<ArrayLitExpr>(stmt->iterable);
  bool isBounded = lit != nullptr && element->isInt() && stmt->binding.sym != nullptr &&
                   !stmt->binding.destructured() && !lit->elements.empty();
  if (!isBounded) {
    return;
  }
  IntegerRange range = this->literalRange(lit);
  if (!range.lo.has_value()) {
    return;
  }
  Term self = Term::of(stmt->binding.sym);
  this->state.getFlowState().getFacts().add(Term::zero(), self, Facts::saturatingSub(1, *range.lo));
  this->state.getFlowState().getFacts().add(self, Term::zero(), Facts::saturatingAdd(*range.hi, 1));
}

IntegerRange LoopChecker::literalRange(const ArrayLitExpr *lit) const {
  IntegerRange range;
  for (const ArrayLitExpr::Element &e : lit->elements) {
    std::optional<int64_t> c =
        e.spread || e.hole ? std::nullopt : this->constantFolder->intConstant(e.expr);
    if (!c.has_value()) {
      return {};
    }
    range.lo = range.lo.has_value() ? std::min(*range.lo, *c) : *c;
    range.hi = range.hi.has_value() ? std::max(*range.hi, *c) : *c;
  }

  return range;
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
  if (type->isRuntimeObject() && this->generatorChecker->asyncIteratedType(type)) {
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
  bool isFitting = !stmt->binding.destructured() || stmt->binding.objectPattern() ||
                   element->isArray() || element->isTuple() || element->isMatch() ||
                   element->isError();
  if (!isFitting) {
    std::string message = ErrorTextLoopShape::notArray(element->name());

    return err(LoopCheckerBindingShapeError{
        .code = LoopCheckerBindingShapeErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(element);
}

void LoopChecker::walkLoopBody(Stmt *body, bool awaits) {
  this->jumpChecker->pushTarget(JumpTarget::Kind::Loop);
  if (awaits) {
    this->state.getFunctionStack().enterAwaitLoop();
  }
  StmtChecker::ResultBodyIsStatement checked = this->stmtChecker->checkBody(body);
  if (checked.isErr()) {
    this->state.getDiagnostics().error(body->loc(), checked.error().message);
  }
  if (awaits) {
    this->state.getFunctionStack().leaveAwaitLoop();
  }
  this->state.getBlockPosition().leaveJumpTarget();
}

}
