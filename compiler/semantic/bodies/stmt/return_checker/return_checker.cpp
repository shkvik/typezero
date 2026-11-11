#include "compiler/semantic/bodies/stmt/return_checker/return_checker.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/truth_checker/truth_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker_errors.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/condition_facts/condition_facts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/destination/destination.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

class Exits {
public:
  Exits(bool forLoop, llvm::ArrayRef<llvm::StringRef> ownLabels);

  bool in(llvm::ArrayRef<const Stmt *> stmts) const;

private:
  bool breakLeaves(const BreakStmt *exit, const TreePlace &place) const;
  bool continueLeaves(const ContinueStmt *next, const TreePlace &place) const;

  bool isLoop;
  llvm::ArrayRef<llvm::StringRef> labels;
};

Exits::Exits(bool forLoop, llvm::ArrayRef<llvm::StringRef> ownLabels)
    : isLoop(forLoop),
      labels(ownLabels) {}

bool Exits::in(llvm::ArrayRef<const Stmt *> stmts) const {
  PlacedNodes nodes = PlacedNodes::collect(stmts, {});
  for (const PlacedStmt &placed : nodes.getStmts()) {
    if (placed.place.isInsideFunction()) {
      continue;
    }
    auto *exit = dyn_cast<BreakStmt>(placed.stmt);
    if (exit != nullptr && this->breakLeaves(exit, placed.place)) {
      return true;
    }
    auto *next = dyn_cast<ContinueStmt>(placed.stmt);
    if (next != nullptr && this->continueLeaves(next, placed.place)) {
      return true;
    }
  }

  return false;
}

bool Exits::breakLeaves(const BreakStmt *exit, const TreePlace &place) const {
  if (!exit->label.empty()) {
    return !place.hasLabel(exit->label);
  }

  return !place.isInsideLoop() && !place.isInsideSwitch();
}

bool Exits::continueLeaves(const ContinueStmt *next, const TreePlace &place) const {
  if (next->label.empty()) {
    return !place.isInsideLoop() && !this->isLoop;
  }
  if (place.hasLabel(next->label)) {
    return false;
  }
  bool ownLabel = llvm::is_contained(this->labels, next->label);

  return !(this->isLoop && ownLabel);
}
}

bool ReturnChecker::isTrueLiteral(const Expr *expr) {
  auto *lit = llvm::dyn_cast_or_null<BoolLitExpr>(expr);

  return lit && lit->value;
}

bool ReturnChecker::breaksTo(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels) {
  if (labels.empty()) {
    return false;
  }
  PlacedNodes nodes = PlacedNodes::collect({stmt}, {});
  for (const PlacedStmt &placed : nodes.getStmts()) {
    if (placed.place.isInsideFunction()) {
      continue;
    }
    auto *exit = dyn_cast<BreakStmt>(placed.stmt);
    if (exit != nullptr && llvm::is_contained(labels, exit->label)) {
      return true;
    }
  }

  return false;
}

ReturnChecker::ReturnChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->byteProofs = &all.byteProofs;
  this->classLinks = &all.classLinks;
  this->exprChecker = &all.exprChecker;
  this->classChecker = &all.classChecker;
  this->truthChecker = &all.truthChecker;
  this->resultChecker = &all.resultChecker;
  this->typeRelations = &all.typeRelations;
  this->conditionFacts = &all.conditionFacts;
  this->emitterChecker = &all.emitterChecker;
  this->predicateChecker = &all.predicateChecker;
}

Expr *ReturnChecker::checkReturnValue(ReturnStmt *ret, const Type *hint, bool tested) {
  if (!this->state.getPredicateStore().isBuildingPredicate()) {
    return this->exprChecker->checkExpr(ret->value, hint);
  }
  if (!tested) {
    Expr *value = this->exprChecker->checkExpr(ret->value, hint);
    if (value->type->isBool()) {
      Flow whenTrue = this->state.getFlowState().getFlow();
      Flow whenFalse = this->state.getFlowState().getFlow();
      this->conditionFacts->addConditionFacts(value, whenTrue, whenFalse);
      PredicateChecker::ResultExitNarrowed narrowed =
          this->predicateChecker->notePredicateExit(whenTrue, whenFalse);
      if (narrowed.isErr()) {
        this->state.getDiagnostics().error(ret->value->loc(), narrowed.error().message);
      }
    }
    return value;
  }
  Flow whenTrue, whenFalse;
  Expr *value = this->truthChecker->checkBranching(ret->value, whenTrue, whenFalse);
  this->state.getFlowState().replaceFlow(FlowChecker::join(whenTrue, whenFalse));
  PredicateChecker::ResultExitNarrowed narrowed =
      this->predicateChecker->notePredicateExit(whenTrue, whenFalse);
  if (narrowed.isErr()) {
    this->state.getDiagnostics().error(ret->value->loc(), narrowed.error().message);
  }

  return value;
}

ReturnChecker::ResultInsideOfFunction ReturnChecker::enclosingFunction() const {
  if (!this->state.getFunctionStack().getCurrentFunction()) {
    std::string message = ErrorTextReturnPlace::outside();

    return err(ReturnCheckerEnclosingFunctionError{
        .code = ReturnCheckerEnclosingFunctionErrorCode::OUTSIDE_FUNCTION,
        .message = message,
    });
  }

  return ok(this->state.getFunctionStack().getCurrentFunction());
}

ReturnChecker::ResultConstructorExits ReturnChecker::constructorReturn(const ReturnStmt *ret,
                                                                       const FuncSymbol *fn) const {
  if (ret->value) {
    std::string message = ErrorTextConstructorReturn::value();

    return err(ReturnCheckerConstructorReturnError{
        .code = ReturnCheckerConstructorReturnErrorCode::VALUE_RETURNED,
        .message = message,
    });
  }
  bool initializer = this->state.getClassContext().getInitializer() == fn->decl;
  if (initializer && this->state.getFlowState().isBeforeSuper()) {
    std::string message = ErrorTextConstructorReturn::early();

    return err(ReturnCheckerConstructorReturnError{
        .code = ReturnCheckerConstructorReturnErrorCode::BEFORE_SUPER,
        .message = message,
    });
  }
  if (initializer) {
    ClassChecker::ResultEveryFieldSet fieldsSet = this->classChecker->checkFieldsSet();
    if (fieldsSet.isErr()) {
      return err(ReturnCheckerConstructorReturnError{
          .code = ReturnCheckerConstructorReturnErrorCode::FIELDS_UNSET,
          .message = fieldsSet.error().message,
      });
    }
  }

  return ok(fn);
}

ReturnChecker::ResultAwaitNotReturned ReturnChecker::awaitReturned(const ReturnStmt *ret,
                                                                   const FuncSymbol *fn) {
  bool awaited = fn->decl && fn->decl->isAsync && !fn->decl->isGenerator && ret->value &&
                 isa<AwaitExpr>(ret->value);
  if (awaited && fn->decl->concise()) {
    std::string message = ErrorTextReturnAwait::concise();

    return err(ReturnCheckerAwaitReturnedError{
        .code = ReturnCheckerAwaitReturnedErrorCode::CONCISE_AWAIT,
        .message = message,
    });
  }
  if (awaited) {
    std::string message = ErrorTextReturnAwait::returned();

    return err(ReturnCheckerAwaitReturnedError{
        .code = ReturnCheckerAwaitReturnedErrorCode::RETURN_AWAIT,
        .message = message,
    });
  }

  return ok(ret);
}

ReturnChecker::ResultValueNotRequired ReturnChecker::valueRequired(const Type *returned,
                                                                   const FuncSymbol *fn) {
  if (!returned->isVoid() && !returned->isError() && !returned->isOptional()) {
    std::string label = ErrorTextFunctionName::label(fn->name);
    std::string message = ErrorTextReturnValue::missing(label, returned->name());

    return err(ReturnCheckerValueRequiredError{
        .code = ReturnCheckerValueRequiredErrorCode::MISSING_VALUE,
        .message = message,
    });
  }

  return ok(returned);
}

ReturnChecker::ResultGeneratorReturns
ReturnChecker::generatorReturn(const Type *given, const Type *returned, const FuncSymbol *fn) {
  bool wrong = given && !given->isUndefined() && !given->isVoid() &&
               (returned->isVoid() || !TypeRelations::isImplicitlyConvertible(given, returned));
  if (wrong) {
    std::string label = ErrorTextFunctionName::label(fn->name);
    std::string message = ErrorTextReturnValue::wrong(label, returned->name(), given->name());

    return err(ReturnCheckerGeneratorReturnError{
        .code = ReturnCheckerGeneratorReturnErrorCode::WRONG_TYPE,
        .message = message,
    });
  }

  return ok(returned);
}

ReturnChecker::ResultPromiseTypeMatch
ReturnChecker::promiseReturn(const Type *given, const Type *returned, const FuncSymbol *fn) {
  if (given != returned && !given->isNever()) {
    std::string label = ErrorTextFunctionName::label(fn->name);
    std::string message = ErrorTextReturnValue::promise(given->name(), label, returned->name());

    return err(ReturnCheckerPromiseReturnError{
        .code = ReturnCheckerPromiseReturnErrorCode::OTHER_PROMISE_TYPE,
        .message = message,
    });
  }

  return ok(given);
}

ReturnChecker::ResultVoidReturnsEmpty ReturnChecker::voidReturn(const Type *type,
                                                                const FuncSymbol *fn) {
  if (!type->isVoid() && !type->isError() && !type->isUndefined() && !fn->discardsResult) {
    std::string label = ErrorTextFunctionName::label(fn->name);
    std::string message = ErrorTextReturnValue::notVoid(label, type->name());

    return err(ReturnCheckerVoidReturnError{
        .code = ReturnCheckerVoidReturnErrorCode::VALUE_FROM_VOID,
        .message = message,
    });
  }

  return ok(type);
}

void ReturnChecker::checkReturn(ReturnStmt *ret) {
  ResultInsideOfFunction inside = this->enclosingFunction();
  if (inside.isErr()) {
    this->state.getDiagnostics().error(ret->loc(), inside.error().message);
    if (ret->value != nullptr) {
      ret->value = this->exprChecker->checkExpr(ret->value);
    }
    return;
  }
  this->emitterChecker->reportUnlistenedErrors();
  const FuncSymbol *fn = inside.value();
  if (fn->decl != nullptr && fn->decl->isConstructor()) {
    return this->walkConstructorReturn(ret, fn);
  }
  ResultAwaitNotReturned awaited = ReturnChecker::awaitReturned(ret, fn);
  if (awaited.isErr()) {
    this->state.getDiagnostics().error(ret->value->loc(), awaited.error().message);
  }
  const Type *returned = ReturnChecker::declaredReturn(fn);
  if (!fn->hasReturnType()) {
    return this->walkInferredReturn(ret);
  }
  if (ret->value == nullptr) {
    ResultValueNotRequired bare = ReturnChecker::valueRequired(returned, fn);
    if (bare.isErr()) {
      this->state.getDiagnostics().error(ret->loc(), bare.error().message);
    }
    return;
  }
  if (fn->decl != nullptr && fn->decl->isAsync && fn->decl->isGenerator) {
    return this->walkAsyncGeneratorReturn(ret, fn, returned);
  }
  ret->value =
      this->checkReturnValue(ret, returned->isVoid() ? nullptr : returned, returned->isBool());
  bool promised = fn->decl != nullptr && fn->decl->isAsync &&
                  fn->getKnownReturnType()->isPromise() && ret->value->type->isPromise();
  if (promised) {
    return this->walkPromiseReturn(ret, fn, returned);
  }
  if (returned->isVoid()) {
    return this->walkVoidReturn(ret, fn);
  }

  return this->walkValueReturn(ret, fn, returned);
}

const Type *ReturnChecker::declaredReturn(const FuncSymbol *fn) {
  bool generator =
      fn->hasReturnType() && fn->decl != nullptr && fn->decl->isGenerator &&
      (fn->getKnownReturnType()->isIterator() || fn->getKnownReturnType()->isAsyncIterator());
  if (generator) {
    return fn->getKnownReturnType()->returned();
  }
  bool promised = fn->hasReturnType() && fn->decl != nullptr && fn->decl->isAsync &&
                  fn->getKnownReturnType()->isPromise();
  if (promised) {
    return fn->getKnownReturnType()->promised();
  }

  return fn->getKnownReturnType();
}

void ReturnChecker::walkConstructorReturn(ReturnStmt *ret, const FuncSymbol *fn) {
  if (ret->value != nullptr) {
    ret->value = this->exprChecker->checkExpr(ret->value);
  }
  ResultConstructorExits exits = this->constructorReturn(ret, fn);
  if (exits.isErr()) {
    bool valued = exits.error().code == ReturnCheckerConstructorReturnErrorCode::VALUE_RETURNED;
    llvm::SMLoc at = valued ? ret->value->loc() : ret->loc();
    this->state.getDiagnostics().error(at, exits.error().message);
  }
}

void ReturnChecker::walkInferredReturn(ReturnStmt *ret) {
  bool shaped = ret->value != nullptr && Destination::literalShaped(ret->value);
  const Type *hint = shaped ? this->earlierReturnShape() : nullptr;
  if (ret->value != nullptr) {
    bool tested = PredicateChecker::looksBoolean(ret->value);
    ret->value = this->checkReturnValue(ret, hint, tested);
  }
  this->state.getFunctionStack().addCollectedReturn(ret);
}

const Type *ReturnChecker::earlierReturnShape() const {
  for (const ReturnStmt *earlier : this->state.getFunctionStack().getCollectedReturns()) {
    if (earlier->value == nullptr || earlier->value->type->isError()) {
      continue;
    }
    const Type *type = earlier->value->type;
    if (type->holdsObjects() || type->isResult()) {
      return type;
    }
  }

  return nullptr;
}

void ReturnChecker::walkAsyncGeneratorReturn(ReturnStmt *ret, const FuncSymbol *fn,
                                             const Type *returned) {
  ret->value = this->exprChecker->checkExpr(ret->value, returned->isVoid() ? nullptr : returned);
  const Type *given =
      ret->value->type->isError() ? nullptr : AsyncChecker::awaitedType(ret->value->type);
  ResultGeneratorReturns gives = ReturnChecker::generatorReturn(given, returned, fn);
  if (gives.isErr()) {
    this->state.getDiagnostics().error(ret->value->loc(), gives.error().message);
  }
}

void ReturnChecker::walkPromiseReturn(ReturnStmt *ret, const FuncSymbol *fn, const Type *returned) {
  const Type *given = ret->value->type->promised();
  ResultPromiseTypeMatch match = ReturnChecker::promiseReturn(given, returned, fn);
  if (match.isErr()) {
    this->state.getDiagnostics().error(ret->value->loc(), match.error().message);
  }
}

void ReturnChecker::walkVoidReturn(ReturnStmt *ret, const FuncSymbol *fn) {
  ResultVoidReturnsEmpty empty = ReturnChecker::voidReturn(ret->value->type, fn);
  if (empty.isErr()) {
    this->state.getDiagnostics().error(ret->value->loc(), empty.error().message);
    this->classLinks->noteInheritedReturn(fn->decl);
    return;
  }
  if (!fn->discardsResult) {
    return;
  }
  ResultChecker::ResultValueGetsUsed used = this->resultChecker->usedValue(ret->value, true);
  if (used.isErr()) {
    this->state.getDiagnostics().error(ret->value->loc(), used.error().message);
  }
}

void ReturnChecker::walkValueReturn(ReturnStmt *ret, const FuncSymbol *fn, const Type *returned) {
  unsigned errors = this->state.getDiagnostics().errorCount();
  ret->value = this->typeRelations->coerce(ret->value, returned);
  if (this->state.getDiagnostics().errorCount() > errors) {
    this->classLinks->noteInheritedReturn(fn->decl);
    return;
  }
  if (fn->decl != nullptr) {
    this->byteProofs->checkByteCallbackResult(fn->decl, ret->value);
  }
}

bool ReturnChecker::isExit(const Stmt *stmt) {
  auto *s = dyn_cast<ExprStmt>(stmt);
  auto *call = s ? dyn_cast<CallExpr>(s->expr) : nullptr;

  return call && (call->builtin() == Builtin::ProcessExit || call->builtin() == Builtin::LibPanic);
}

bool ReturnChecker::alwaysReturns(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels) {
  ReturnPart whole{stmt, std::vector<llvm::StringRef>(labels.begin(), labels.end())};
  std::vector<bool> answers;
  for (const ReturnPart &part : ChildrenFirst::order(whole, ReturnChecker::returnParts)) {
    bool returns = ReturnChecker::nodeReturns(part, answers);
    answers.push_back(returns);
  }

  return answers.back();
}

NodeChildren<ReturnPart> ReturnChecker::returnParts(ReturnPart part) {
  NodeChildren<const Stmt *> inner;
  if (auto *block = dyn_cast<BlockStmt>(part.stmt)) {
    inner.append(block->body.begin(), block->body.end());
  } else if (auto *branch = dyn_cast<IfStmt>(part.stmt)) {
    inner = ReturnChecker::ifBranches(branch);
  } else if (auto *cases = dyn_cast<SwitchStmt>(part.stmt)) {
    const std::vector<Stmt *> *tail = ReturnChecker::switchTail(cases, part.labels);
    if (tail != nullptr) {
      inner.append(tail->begin(), tail->end());
    }
  } else if (isa<LabeledStmt>(part.stmt)) {
    return {ReturnChecker::unlabeled(part)};
  }
  NodeChildren<ReturnPart> parts;
  for (const Stmt *stmt : inner) {
    parts.push_back(ReturnPart{stmt, {}});
  }

  return parts;
}

ReturnPart ReturnChecker::unlabeled(const ReturnPart &part) {
  ReturnPart inner = part;
  while (auto *labeled = dyn_cast<LabeledStmt>(inner.stmt)) {
    inner.labels.push_back(labeled->label);
    inner.stmt = labeled->body;
  }

  return inner;
}

NodeChildren<const Stmt *> ReturnChecker::ifBranches(const IfStmt *s) {
  if (auto *lit = dyn_cast<BoolLitExpr>(s->cond)) {
    if (lit->value && !s->elseStmt) {
      return {s->thenStmt};
    }
    if (!lit->value && isa<EmptyStmt>(s->thenStmt)) {
      return s->elseStmt ? NodeChildren<const Stmt *>{s->elseStmt} : NodeChildren<const Stmt *>{};
    }
  }
  if (!s->elseStmt) {
    return {};
  }

  return {s->thenStmt, s->elseStmt};
}

const std::vector<Stmt *> *ReturnChecker::switchTail(const SwitchStmt *s,
                                                     llvm::ArrayRef<llvm::StringRef> labels) {
  bool hasDefault =
      s->exhaustive || llvm::any_of(s->cases, [](const SwitchCase &c) { return !c.test; });
  if (!hasDefault || s->cases.empty()) {
    return nullptr;
  }
  Exits exits(false, labels);
  if (llvm::any_of(s->cases, [&](const SwitchCase &c) { return exits.in(c.body); })) {
    return nullptr;
  }

  return &s->cases.back().body;
}

bool ReturnChecker::nodeReturns(const ReturnPart &part, std::vector<bool> &answers) {
  const Stmt *stmt = part.stmt;
  llvm::ArrayRef<llvm::StringRef> labels = part.labels;
  switch (stmt->kind()) {
  case Stmt::Kind::Return:
    return true;
  case Stmt::Kind::Expr:
    return ReturnChecker::isExit(stmt);
  case Stmt::Kind::Block: {
    std::vector<bool> inner =
        ChildrenFirst::takeAnswers(answers, cast<BlockStmt>(stmt)->body.size());
    return llvm::is_contained(inner, true) && !ReturnChecker::breaksTo(stmt, labels);
  }
  case Stmt::Kind::If: {
    size_t count = ReturnChecker::ifBranches(cast<IfStmt>(stmt)).size();
    std::vector<bool> inner = ChildrenFirst::takeAnswers(answers, count);
    return count > 0 && !llvm::is_contained(inner, false) && !ReturnChecker::breaksTo(stmt, labels);
  }
  case Stmt::Kind::While: {
    auto *s = cast<WhileStmt>(stmt);
    return ReturnChecker::isTrueLiteral(s->cond) && !Exits(true, labels).in({s->body});
  }
  case Stmt::Kind::DoWhile: {
    auto *s = cast<DoWhileStmt>(stmt);
    return ReturnChecker::isTrueLiteral(s->cond) && !Exits(true, labels).in({s->body});
  }
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    return (!s->cond || ReturnChecker::isTrueLiteral(s->cond)) &&
           !Exits(true, labels).in({s->body});
  }
  case Stmt::Kind::Switch: {
    const std::vector<Stmt *> *tail = ReturnChecker::switchTail(cast<SwitchStmt>(stmt), labels);
    if (tail == nullptr) {
      return false;
    }
    std::vector<bool> inner = ChildrenFirst::takeAnswers(answers, tail->size());
    return llvm::is_contained(inner, true);
  }
  case Stmt::Kind::Labeled:
    return ChildrenFirst::takeAnswer(answers);
  default:
    return false;
  }
}

}
