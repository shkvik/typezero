#include "compiler/semantic/bodies/stmt/loop_checker/loop_checker.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
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
  this->flowChecker = &all.flowChecker;
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

bool LoopChecker::isContinuingTo(const Stmt *body, llvm::ArrayRef<llvm::StringRef> labels) const {
  auto nodes = PlacedNodes::collect({body}, {});
  for (const PlacedStmt &placed : nodes.getStmts()) {
    auto *next = dyn_cast<ContinueStmt>(placed.stmt);
    bool isOwn = next != nullptr && !placed.place.isInsideFunction();
    if (isOwn && this->isContinuingLoop(next, placed.place, labels)) {
      return true;
    }
  }

  return false;
}

bool LoopChecker::isContinuingLoop(const ContinueStmt *next, const TreePlace &place,
                                   llvm::ArrayRef<llvm::StringRef> labels) const {
  if (!next->label.empty()) {
    return llvm::is_contained(labels, next->label);
  }

  return !place.isInsideLoop();
}

Flow LoopChecker::flowAfter(const LoopScan &scan, const Flow &whenFalse, const Flow &head) const {
  if (scan.isBreakingOut) {
    return this->flowChecker->join(whenFalse, head);
  }

  return whenFalse;
}

void LoopChecker::walkWhile(WhileStmt *stmt) {
  auto scan = this->loopScanner->scanLoop({stmt->body}, {stmt->cond});
  auto head = this->loopBounds->countedLoopHead(scan, stmt->cond);
  this->state.getFlowState().replaceFlow(head);
  auto whenTrue = Flow();
  auto whenFalse = Flow();
  stmt->cond = this->truthChecker->checkCondition(stmt->cond, whenTrue, whenFalse);
  this->state.getFlowState().replaceFlow(whenTrue);
  this->walkScannedBody(stmt->body, scan);
  auto after = this->flowAfter(scan, whenFalse, head);
  this->state.getFlowState().replaceFlow(after);
}

void LoopChecker::walkDoWhile(DoWhileStmt *stmt) {
  auto scan = this->loopScanner->scanLoop({stmt->body}, {stmt->cond});
  auto head = this->loopBounds->loopHead(scan);
  this->state.getFlowState().replaceFlow(head);
  this->walkScannedBody(stmt->body, scan);
  const Flow &bodyEnd = this->state.getFlowState().getFlow();
  auto beforeCond = this->flowChecker->join(bodyEnd, head);
  this->state.getFlowState().replaceFlow(beforeCond);
  auto whenTrue = Flow();
  auto whenFalse = Flow();
  stmt->cond = this->truthChecker->checkCondition(stmt->cond, whenTrue, whenFalse);
  auto after = this->flowAfter(scan, whenFalse, head);
  this->state.getFlowState().replaceFlow(after);
}

void LoopChecker::walkFor(ForStmt *stmt) {
  auto labels = this->state.getBlockPosition().getPendingLabels();
  this->state.getScopeStack().openScope();
  this->state.getBlockPosition().forgetBlock();
  if (stmt->init != nullptr) {
    this->stmtChecker->walkStmt(stmt->init);
  }
  auto scan = this->loopScanner->scanLoop({stmt->body}, {stmt->cond, stmt->step});
  auto head = this->loopBounds->countedLoopHead(scan, stmt->cond);
  this->state.getFlowState().replaceFlow(head);
  auto whenTrue = head;
  auto whenFalse = head;
  whenFalse.unreachable = true;
  if (stmt->cond != nullptr) {
    stmt->cond = this->truthChecker->checkCondition(stmt->cond, whenTrue, whenFalse);
  }
  this->state.getFlowState().replaceFlow(whenTrue);
  this->walkScannedBody(stmt->body, scan);
  if (stmt->step != nullptr) {
    this->enterStepFlow(stmt->body, labels, head, whenTrue);
    this->walkForStep(stmt);
  }
  auto after = this->flowAfter(scan, whenFalse, head);
  this->state.getFlowState().replaceFlow(after);
  this->scopes->closeScope();
}

void LoopChecker::enterStepFlow(const Stmt *body, llvm::ArrayRef<llvm::StringRef> labels,
                                const Flow &head, const Flow &whenTrue) {
  if (this->isContinuingTo(body, labels)) {
    this->state.getFlowState().replaceFlow(head);
    return;
  }
  if (this->state.getFlowState().isUnreachable()) {
    this->state.getFlowState().replaceFlow(whenTrue);
  }
}

void LoopChecker::walkForStep(ForStmt *stmt) {
  auto *yield = dyn_cast<YieldExpr>(stmt->step);
  if (yield != nullptr) {
    yield->discarded = true;
  }
  stmt->step = this->exprChecker->checkExpr(stmt->step);
  auto used = this->resultChecker->checkUsedValue(stmt->step);
  if (used.isErr()) {
    this->state.getDiagnostics().error(stmt->step->loc(), used.error().message);
  }
}

void LoopChecker::walkForOf(ForOfStmt *stmt) {
  if (stmt->isAwait) {
    this->asyncChecker->walkForAwait(stmt);
    this->emitterChecker->noteErrorsHandled(stmt->iterable);
    return;
  }
  this->walkSource(stmt);
  const Type *type = stmt->iterable->type;
  const auto *iterated = this->iteratedElement(stmt);
  const Type *element = this->boundElement(stmt, iterated);
  auto scan = this->loopScanner->scanForOfLoop(stmt);
  if (type->isIterator()) {
    scan.isCallingFunctions = true;
  }
  bool isOwnedList = stmt->isConst && type->isArray() && element->isManaged();
  stmt->borrowElements = isOwnedList && !scan.isCallingFunctions && !scan.isReleasingElements;
  auto head = this->loopBounds->loopHead(scan);
  this->state.getFlowState().replaceFlow(head);
  this->state.getScopeStack().openScope();
  VarDeclarator &binding = stmt->binding;
  binding.sym = this->scopes->declareVar(binding.name, binding.loc, element, stmt->isConst);
  this->addLiteralBounds(stmt, element);
  this->walkScannedBody(stmt->body, scan);
  this->scopes->closeScope();
  this->state.getFlowState().replaceFlow(head);
}

void LoopChecker::walkSource(ForOfStmt *stmt) {
  if (stmt->keys) {
    this->objectChecker->walkForIn(stmt);
  } else {
    this->walkIterable(stmt);
  }
  if (stmt->iterable->type->isMatch()) {
    stmt->iterable = this->matchChecker->matchItems(stmt->iterable);
  }
}

const Type *LoopChecker::boundElement(const ForOfStmt *stmt, const Type *element) {
  auto typed = this->bindingType(stmt);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(stmt->binding.typeAnn->loc, typed.error().message);
  }
  auto shape = this->bindingShape(stmt, element);
  if (shape.isErr()) {
    this->state.getDiagnostics().error(stmt->binding.loc, shape.error().message);

    return Type::error();
  }

  return element;
}

void LoopChecker::walkIterable(ForOfStmt *stmt) {
  this->iteratorMethods->markFused(stmt->iterable);
  auto *checked = this->exprChecker->checkExpr(stmt->iterable);
  auto *apparent = this->genericChecker->apparent(checked);
  stmt->iterable = this->dictChecker->iterableOf(apparent);
  auto *name = dyn_cast<NameExpr>(stmt->iterable);
  if (name == nullptr || !this->isListParam(name)) {
    return;
  }
  auto *elements = this->listElements(name);
  if (elements != nullptr) {
    stmt->iterable = elements;
  }
}

bool LoopChecker::isListParam(const NameExpr *name) {
  bool isTuple =
      name->var() != nullptr && name->type->isTuple() && name->type->tupleRest() == nullptr;
  if (!isTuple) {
    return false;
  }
  for (const FunctionDecl *functionDecl : this->state.getFunctionStack().getFunctions()) {
    if (functionDecl->listName == name->name && name->var()->owner() == functionDecl) {
      return true;
    }
  }

  return false;
}

Expr *LoopChecker::listElements(const NameExpr *name) {
  auto &ast = this->state.getAst();
  llvm::SMLoc loc = name->loc();
  auto *elements = ast.make<ArrayLitExpr>(loc);
  auto kinds = std::vector<const Type *>();
  for (const Field &field : name->type->fields()) {
    kinds.push_back(field.type);
  }
  for (size_t index = 0; index < kinds.size(); ++index) {
    auto *root = ast.make<NameExpr>(loc, name->name);
    auto *position = ast.make<IntLitExpr>(loc, index);
    auto *item = ast.make<IndexExpr>(loc, root, position);
    auto element = ArrayLitExpr::Element{.expr = item, .spread = false};
    elements->elements.push_back(element);
  }
  if (kinds.empty()) {
    return nullptr;
  }
  const Type *element = Type::unionOf(kinds);
  bool isTyped = element != nullptr && UnionSlots::unionProblem(element).empty();
  const Type *expected = isTyped ? Type::array(element) : nullptr;

  return this->exprChecker->checkExpr(elements, expected);
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
    auto label = ErrorTextForOf::loop();
    auto nothing = this->generatorChecker->checkSendsNothing(type, label);
    if (nothing.isErr()) {
      this->state.getDiagnostics().error(stmt->iterable->loc(), nothing.error().message);
    }

    return type->yielded();
  }
  auto *copy = this->tupleChecker->arrayCopy(stmt->iterable);
  if (copy != nullptr) {
    stmt->iterable = copy;

    return copy->type->inner();
  }
  auto iterable = this->iterableType(type);
  if (iterable.isErr()) {
    this->state.getDiagnostics().error(stmt->iterable->loc(), iterable.error().message);
  }

  return Type::error();
}

void LoopChecker::addLiteralBounds(const ForOfStmt *stmt, const Type *element) {
  auto *literal = dyn_cast<ArrayLitExpr>(stmt->iterable);
  bool isBounded = literal != nullptr && element->isInt() && stmt->binding.sym != nullptr &&
                   !stmt->binding.destructured() && !literal->elements.empty();
  if (!isBounded) {
    return;
  }
  auto range = this->literalRange(literal);
  if (!range.lo.has_value()) {
    return;
  }
  auto &facts = this->state.getFlowState().getFacts();
  auto self = Term::of(stmt->binding.sym);
  auto belowLowest = Facts::saturatingSub(1, *range.lo);
  auto aboveHighest = Facts::saturatingAdd(*range.hi, 1);
  facts.add(Term::zero(), self, belowLowest);
  facts.add(self, Term::zero(), aboveHighest);
}

IntegerRange LoopChecker::literalRange(const ArrayLitExpr *literal) const {
  IntegerRange range;
  for (const ArrayLitExpr::Element &element : literal->elements) {
    auto value = this->elementConstant(element);
    if (!value.has_value()) {
      return {};
    }
    range.lo = range.lo.has_value() ? std::min(*range.lo, *value) : *value;
    range.hi = range.hi.has_value() ? std::max(*range.hi, *value) : *value;
  }

  return range;
}

std::optional<int64_t> LoopChecker::elementConstant(const ArrayLitExpr::Element &element) const {
  if (element.spread || element.hole) {
    return std::nullopt;
  }

  return this->constantFolder->intConstant(element.expr);
}

LoopChecker::ResultIterableType LoopChecker::iterableType(const Type *type) const {
  if (this->isOptionalIterable(type)) {
    auto message = ErrorTextForOf::mayBeUndef(type->name());

    return err(LoopCheckerIterableTypeError{
        .code = LoopCheckerIterableTypeErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type->isTuple()) {
    auto message = ErrorTextForOf::tuple();

    return err(LoopCheckerIterableTypeError{
        .code = LoopCheckerIterableTypeErrorCode::TUPLE_NOT_ITERABLE,
        .message = message,
    });
  }
  if (type->isRecord()) {
    auto message = ErrorTextForOf::record();

    return err(LoopCheckerIterableTypeError{
        .code = LoopCheckerIterableTypeErrorCode::RECORD_NOT_ITERABLE,
        .message = message,
    });
  }
  if (this->isAsyncOnly(type)) {
    auto message = ErrorTextForOf::later(type->name());

    return err(LoopCheckerIterableTypeError{
        .code = LoopCheckerIterableTypeErrorCode::ASYNC_ONLY,
        .message = message,
    });
  }
  if (this->isConcurrentMap(type)) {
    auto message = ErrorTextForOf::concurrent();

    return err(LoopCheckerIterableTypeError{
        .code = LoopCheckerIterableTypeErrorCode::CONCURRENT_MAP,
        .message = message,
    });
  }
  if (!type->isError()) {
    auto message = ErrorTextForOf::cannot(type->name());

    return err(LoopCheckerIterableTypeError{
        .code = LoopCheckerIterableTypeErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(type);
}

bool LoopChecker::isOptionalIterable(const Type *type) const {
  if (!type->isOptional()) {
    return false;
  }
  const auto *element = this->generatorChecker->iteratedType(type->inner());

  return element != nullptr;
}

bool LoopChecker::isAsyncOnly(const Type *type) const {
  if (!type->isRuntimeObject()) {
    return false;
  }
  const auto *element = this->generatorChecker->asyncIteratedType(type);

  return element != nullptr;
}

bool LoopChecker::isConcurrentMap(const Type *type) const {
  const ClassDecl *classDecl = type->classDecl();
  if (classDecl == nullptr || !classDecl->library() || classDecl->generic() == nullptr) {
    return false;
  }

  return LibraryClassTable::isName(classDecl->generic()->name, LibraryClass::CONCURRENT_MAP);
}

LoopChecker::ResultBindingTyped LoopChecker::bindingType(const ForOfStmt *stmt) const {
  bool isAnnotated = stmt->binding.typeAnn.has_value();
  if (isAnnotated && stmt->keys) {
    auto message = ErrorTextLoopBinding::forIn();

    return err(LoopCheckerBindingTypeError{
        .code = LoopCheckerBindingTypeErrorCode::FOR_IN_ANNOTATED,
        .message = message,
    });
  }
  if (isAnnotated) {
    auto message = ErrorTextLoopBinding::forOf();

    return err(LoopCheckerBindingTypeError{
        .code = LoopCheckerBindingTypeErrorCode::FOR_OF_ANNOTATED,
        .message = message,
    });
  }

  return ok(&stmt->binding);
}

LoopChecker::ResultBindingShape LoopChecker::bindingShape(const ForOfStmt *stmt,
                                                          const Type *element) const {
  bool isListLike =
      element->isArray() || element->isTuple() || element->isMatch() || element->isError();
  bool isFitting = !stmt->binding.destructured() || stmt->binding.objectPattern() || isListLike;
  if (!isFitting) {
    auto message = ErrorTextLoopShape::notArray(element->name());

    return err(LoopCheckerBindingShapeError{
        .code = LoopCheckerBindingShapeErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(element);
}

void LoopChecker::walkScannedBody(Stmt *body, const LoopScan &scan) {
  if (scan.hasAwait) {
    return this->walkAwaitLoopBody(body);
  }

  return this->walkLoopBody(body);
}

void LoopChecker::walkLoopBody(Stmt *body) {
  this->jumpChecker->pushTarget(JumpTarget::Kind::Loop);
  this->walkBody(body);
  this->state.getBlockPosition().leaveJumpTarget();
}

void LoopChecker::walkAwaitLoopBody(Stmt *body) {
  this->jumpChecker->pushTarget(JumpTarget::Kind::Loop);
  this->state.getFunctionStack().enterAwaitLoop();
  this->walkBody(body);
  this->state.getFunctionStack().leaveAwaitLoop();
  this->state.getBlockPosition().leaveJumpTarget();
}

void LoopChecker::walkBody(Stmt *body) {
  auto checked = this->stmtChecker->checkBody(body);
  if (checked.isErr()) {
    this->state.getDiagnostics().error(body->loc(), checked.error().message);
  }
}

}
