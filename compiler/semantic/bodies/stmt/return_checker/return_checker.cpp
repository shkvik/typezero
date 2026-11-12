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

enum class JumpOwner {
  LOOP,
  SWITCH,
};

class Exits {
public:
  Exits(JumpOwner jumpOwner, llvm::ArrayRef<llvm::StringRef> ownLabels);

  bool hasExitIn(llvm::ArrayRef<const Stmt *> stmts) const;

private:
  bool isLeavingJump(const PlacedStmt &placed) const;
  bool isBreakLeaving(const BreakStmt *exit, const TreePlace &place) const;
  bool isContinueLeaving(const ContinueStmt *next, const TreePlace &place) const;

  JumpOwner owner;
  llvm::ArrayRef<llvm::StringRef> labels;
};

Exits::Exits(JumpOwner jumpOwner, llvm::ArrayRef<llvm::StringRef> ownLabels)
    : owner(jumpOwner),
      labels(ownLabels) {}

bool Exits::hasExitIn(llvm::ArrayRef<const Stmt *> stmts) const {
  auto nodes = PlacedNodes::collect(stmts, {});
  for (const PlacedStmt &placed : nodes.getStmts()) {
    if (this->isLeavingJump(placed)) {
      return true;
    }
  }

  return false;
}

bool Exits::isLeavingJump(const PlacedStmt &placed) const {
  if (placed.place.isInsideFunction()) {
    return false;
  }
  auto *exit = dyn_cast<BreakStmt>(placed.stmt);
  auto *next = dyn_cast<ContinueStmt>(placed.stmt);
  if (exit != nullptr) {
    return this->isBreakLeaving(exit, placed.place);
  }
  if (next != nullptr) {
    return this->isContinueLeaving(next, placed.place);
  }

  return false;
}

bool Exits::isBreakLeaving(const BreakStmt *exit, const TreePlace &place) const {
  if (!exit->label.empty()) {
    return !place.hasLabel(exit->label);
  }

  return !place.isInsideLoop() && !place.isInsideSwitch();
}

bool Exits::isContinueLeaving(const ContinueStmt *next, const TreePlace &place) const {
  bool isLoop = this->owner == JumpOwner::LOOP;
  if (next->label.empty()) {
    return !place.isInsideLoop() && !isLoop;
  }
  if (place.hasLabel(next->label)) {
    return false;
  }
  bool isOwnLabel = llvm::is_contained(this->labels, next->label);

  return !(isLoop && isOwnLabel);
}
}

bool ReturnChecker::isTrueLiteral(const Expr *expr) const {
  auto *literal = llvm::dyn_cast_or_null<BoolLitExpr>(expr);

  return literal != nullptr && literal->value;
}

bool ReturnChecker::isBreakingTo(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels) const {
  if (labels.empty()) {
    return false;
  }
  auto nodes = PlacedNodes::collect({stmt}, {});
  for (const PlacedStmt &placed : nodes.getStmts()) {
    if (this->isLabeledBreak(placed, labels)) {
      return true;
    }
  }

  return false;
}

bool ReturnChecker::isLabeledBreak(const PlacedStmt &placed,
                                   llvm::ArrayRef<llvm::StringRef> labels) const {
  auto *exit = dyn_cast<BreakStmt>(placed.stmt);
  if (placed.place.isInsideFunction() || exit == nullptr) {
    return false;
  }

  return llvm::is_contained(labels, exit->label);
}

ReturnChecker::ReturnChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->byteProofs = &all.byteProofs;
  this->classLinks = &all.classLinks;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->destination = &all.destination;
  this->classChecker = &all.classChecker;
  this->truthChecker = &all.truthChecker;
  this->resultChecker = &all.resultChecker;
  this->typeRelations = &all.typeRelations;
  this->conditionFacts = &all.conditionFacts;
  this->emitterChecker = &all.emitterChecker;
  this->predicateChecker = &all.predicateChecker;
}

Expr *ReturnChecker::checkReturnValue(ReturnStmt *returnStmt, const Type *hint, bool isTested) {
  if (!this->state.getPredicateStore().isBuildingPredicate()) {
    return this->exprChecker->checkExpr(returnStmt->value, hint);
  }
  if (isTested) {
    return this->checkTestedReturn(returnStmt);
  }

  return this->checkPredicateReturn(returnStmt, hint);
}

Expr *ReturnChecker::checkPredicateReturn(ReturnStmt *returnStmt, const Type *hint) {
  auto *value = this->exprChecker->checkExpr(returnStmt->value, hint);
  if (!value->type->isBool()) {
    return value;
  }
  auto whenTrue = this->state.getFlowState().getFlow();
  auto whenFalse = this->state.getFlowState().getFlow();
  this->conditionFacts->addConditionFacts(value, whenTrue, whenFalse);
  this->notePredicateExit(returnStmt, whenTrue, whenFalse);

  return value;
}

Expr *ReturnChecker::checkTestedReturn(ReturnStmt *returnStmt) {
  auto whenTrue = Flow();
  auto whenFalse = Flow();
  auto *value = this->truthChecker->checkBranching(returnStmt->value, whenTrue, whenFalse);
  auto joined = this->flowChecker->join(whenTrue, whenFalse);
  this->state.getFlowState().replaceFlow(joined);
  this->notePredicateExit(returnStmt, whenTrue, whenFalse);

  return value;
}

void ReturnChecker::notePredicateExit(const ReturnStmt *returnStmt, const Flow &whenTrue,
                                      const Flow &whenFalse) {
  auto narrowed = this->predicateChecker->notePredicateExit(whenTrue, whenFalse);
  if (narrowed.isErr()) {
    this->state.getDiagnostics().error(returnStmt->value->loc(), narrowed.error().message);
  }
}

ReturnChecker::ResultInsideOfFunction ReturnChecker::checkInsideFunction() const {
  const auto *function = this->state.getFunctionStack().getCurrentFunction();
  if (function == nullptr) {
    auto message = ErrorTextReturnPlace::outside();

    return err(ReturnCheckerEnclosingFunctionError{
        .code = ReturnCheckerEnclosingFunctionErrorCode::OUTSIDE_FUNCTION,
        .message = message,
    });
  }

  return ok(function);
}

ReturnChecker::ResultConstructorExits
ReturnChecker::checkConstructorReturn(const ReturnStmt *returnStmt,
                                      const FuncSymbol *function) const {
  if (returnStmt->value != nullptr) {
    auto message = ErrorTextConstructorReturn::value();

    return err(ReturnCheckerConstructorReturnError{
        .code = ReturnCheckerConstructorReturnErrorCode::VALUE_RETURNED,
        .message = message,
    });
  }
  bool isInitializer = this->state.getClassContext().getInitializer() == function->decl;
  if (isInitializer && this->state.getFlowState().isBeforeSuper()) {
    auto message = ErrorTextConstructorReturn::early();

    return err(ReturnCheckerConstructorReturnError{
        .code = ReturnCheckerConstructorReturnErrorCode::BEFORE_SUPER,
        .message = message,
    });
  }
  if (!isInitializer) {
    return ok(function);
  }
  auto fieldsSet = this->classChecker->checkFieldsSet();
  if (fieldsSet.isErr()) {
    return err(ReturnCheckerConstructorReturnError{
        .code = ReturnCheckerConstructorReturnErrorCode::FIELDS_UNSET,
        .message = fieldsSet.error().message,
    });
  }

  return ok(function);
}

ReturnChecker::ResultAwaitNotReturned
ReturnChecker::checkAwaitNotReturned(const ReturnStmt *returnStmt,
                                     const FuncSymbol *function) const {
  const FunctionDecl *functionDecl = function->decl;
  bool isAsyncValue = functionDecl != nullptr && functionDecl->isAsync &&
                      !functionDecl->isGenerator && returnStmt->value != nullptr;
  bool isAwaited = isAsyncValue && isa<AwaitExpr>(returnStmt->value);
  if (isAwaited && functionDecl->concise()) {
    auto message = ErrorTextReturnAwait::concise();

    return err(ReturnCheckerAwaitReturnedError{
        .code = ReturnCheckerAwaitReturnedErrorCode::CONCISE_AWAIT,
        .message = message,
    });
  }
  if (isAwaited) {
    auto message = ErrorTextReturnAwait::returned();

    return err(ReturnCheckerAwaitReturnedError{
        .code = ReturnCheckerAwaitReturnedErrorCode::RETURN_AWAIT,
        .message = message,
    });
  }

  return ok(returnStmt);
}

ReturnChecker::ResultValueNotRequired
ReturnChecker::checkValueNotRequired(const Type *returned, const FuncSymbol *function) const {
  if (!returned->isVoid() && !returned->isError() && !returned->isOptional()) {
    auto label = ErrorTextFunctionName::label(function->name);
    auto message = ErrorTextReturnValue::missing(label, returned->name());

    return err(ReturnCheckerValueRequiredError{
        .code = ReturnCheckerValueRequiredErrorCode::MISSING_VALUE,
        .message = message,
    });
  }

  return ok(returned);
}

ReturnChecker::ResultGeneratorReturns
ReturnChecker::checkGeneratorReturn(const Type *given, const Type *returned,
                                    const FuncSymbol *function) const {
  bool isGiven = given != nullptr && !given->isUndefined() && !given->isVoid();
  bool isWrong =
      isGiven && (returned->isVoid() || !TypeRelations::isImplicitlyConvertible(given, returned));
  if (isWrong) {
    auto label = ErrorTextFunctionName::label(function->name);
    auto message = ErrorTextReturnValue::wrong(label, returned->name(), given->name());

    return err(ReturnCheckerGeneratorReturnError{
        .code = ReturnCheckerGeneratorReturnErrorCode::WRONG_TYPE,
        .message = message,
    });
  }

  return ok(returned);
}

ReturnChecker::ResultPromiseTypeMatch
ReturnChecker::checkPromiseReturn(const Type *given, const Type *returned,
                                  const FuncSymbol *function) const {
  if (given != returned && !given->isNever()) {
    auto label = ErrorTextFunctionName::label(function->name);
    auto message = ErrorTextReturnValue::promise(given->name(), label, returned->name());

    return err(ReturnCheckerPromiseReturnError{
        .code = ReturnCheckerPromiseReturnErrorCode::OTHER_PROMISE_TYPE,
        .message = message,
    });
  }

  return ok(given);
}

ReturnChecker::ResultVoidReturnsEmpty
ReturnChecker::checkVoidReturn(const Type *type, const FuncSymbol *function) const {
  if (!type->isVoid() && !type->isError() && !type->isUndefined() && !function->discardsResult) {
    auto label = ErrorTextFunctionName::label(function->name);
    auto message = ErrorTextReturnValue::notVoid(label, type->name());

    return err(ReturnCheckerVoidReturnError{
        .code = ReturnCheckerVoidReturnErrorCode::VALUE_FROM_VOID,
        .message = message,
    });
  }

  return ok(type);
}

void ReturnChecker::walkReturn(ReturnStmt *returnStmt) {
  auto inside = this->checkInsideFunction();
  if (inside.isErr()) {
    this->state.getDiagnostics().error(returnStmt->loc(), inside.error().message);
    this->walkStrayValue(returnStmt);
    return;
  }
  this->emitterChecker->reportUnlistenedErrors();
  const FuncSymbol *function = inside.value();
  const FunctionDecl *functionDecl = function->decl;
  if (functionDecl != nullptr && functionDecl->isConstructor()) {
    return this->walkConstructorReturn(returnStmt, function);
  }
  auto awaited = this->checkAwaitNotReturned(returnStmt, function);
  if (awaited.isErr()) {
    this->state.getDiagnostics().error(returnStmt->value->loc(), awaited.error().message);
  }
  const auto *returned = this->declaredReturn(function);
  if (!function->hasReturnType()) {
    return this->walkInferredReturn(returnStmt);
  }
  if (returnStmt->value == nullptr) {
    return this->walkBareReturn(returnStmt, function, returned);
  }
  bool isAsyncGenerator =
      functionDecl != nullptr && functionDecl->isAsync && functionDecl->isGenerator;
  if (isAsyncGenerator) {
    return this->walkAsyncGeneratorReturn(returnStmt, function, returned);
  }
  const Type *hint = returned->isVoid() ? nullptr : returned;
  bool isTested = returned->isBool();
  returnStmt->value = this->checkReturnValue(returnStmt, hint, isTested);
  bool isPromised = functionDecl != nullptr && functionDecl->isAsync &&
                    function->getKnownReturnType()->isPromise() &&
                    returnStmt->value->type->isPromise();
  if (isPromised) {
    return this->walkPromiseReturn(returnStmt, function, returned);
  }
  if (returned->isVoid()) {
    return this->walkVoidReturn(returnStmt, function);
  }

  return this->walkValueReturn(returnStmt, function, returned);
}

void ReturnChecker::walkStrayValue(ReturnStmt *returnStmt) {
  if (returnStmt->value != nullptr) {
    returnStmt->value = this->exprChecker->checkExpr(returnStmt->value);
  }
}

void ReturnChecker::walkBareReturn(const ReturnStmt *returnStmt, const FuncSymbol *function,
                                   const Type *returned) {
  auto bare = this->checkValueNotRequired(returned, function);
  if (bare.isErr()) {
    this->state.getDiagnostics().error(returnStmt->loc(), bare.error().message);
  }
}

const Type *ReturnChecker::declaredReturn(const FuncSymbol *function) const {
  const FunctionDecl *functionDecl = function->decl;
  bool isDeclared = function->hasReturnType() && functionDecl != nullptr;
  const Type *known = function->getKnownReturnType();
  bool isGenerator =
      isDeclared && functionDecl->isGenerator && (known->isIterator() || known->isAsyncIterator());
  if (isGenerator) {
    return known->returned();
  }
  bool isPromised = isDeclared && functionDecl->isAsync && known->isPromise();
  if (isPromised) {
    return known->promised();
  }

  return known;
}

void ReturnChecker::walkConstructorReturn(ReturnStmt *returnStmt, const FuncSymbol *function) {
  this->walkStrayValue(returnStmt);
  auto exits = this->checkConstructorReturn(returnStmt, function);
  if (exits.isErr()) {
    bool isValued = exits.error().code == ReturnCheckerConstructorReturnErrorCode::VALUE_RETURNED;
    llvm::SMLoc at = isValued ? returnStmt->value->loc() : returnStmt->loc();
    this->state.getDiagnostics().error(at, exits.error().message);
  }
}

void ReturnChecker::walkInferredReturn(ReturnStmt *returnStmt) {
  bool isShaped =
      returnStmt->value != nullptr && this->destination->isLiteralShaped(returnStmt->value);
  const Type *hint = isShaped ? this->earlierReturnShape() : nullptr;
  if (returnStmt->value != nullptr) {
    bool isTested = this->predicateChecker->isLookingBoolean(returnStmt->value);
    returnStmt->value = this->checkReturnValue(returnStmt, hint, isTested);
  }
  this->state.getFunctionStack().addCollectedReturn(returnStmt);
}

const Type *ReturnChecker::earlierReturnShape() const {
  for (const ReturnStmt *earlier : this->state.getFunctionStack().getCollectedReturns()) {
    if (this->isShapedReturn(earlier)) {
      return earlier->value->type;
    }
  }

  return nullptr;
}

bool ReturnChecker::isShapedReturn(const ReturnStmt *earlier) const {
  if (earlier->value == nullptr || earlier->value->type->isError()) {
    return false;
  }
  const Type *type = earlier->value->type;

  return type->holdsObjects() || type->isResult();
}

void ReturnChecker::walkAsyncGeneratorReturn(ReturnStmt *returnStmt, const FuncSymbol *function,
                                             const Type *returned) {
  const Type *hint = returned->isVoid() ? nullptr : returned;
  returnStmt->value = this->exprChecker->checkExpr(returnStmt->value, hint);
  const Type *valueType = returnStmt->value->type;
  const Type *given = valueType->isError() ? nullptr : AsyncChecker::awaitedType(valueType);
  auto gives = this->checkGeneratorReturn(given, returned, function);
  if (gives.isErr()) {
    this->state.getDiagnostics().error(returnStmt->value->loc(), gives.error().message);
  }
}

void ReturnChecker::walkPromiseReturn(ReturnStmt *returnStmt, const FuncSymbol *function,
                                      const Type *returned) {
  const Type *given = returnStmt->value->type->promised();
  auto match = this->checkPromiseReturn(given, returned, function);
  if (match.isErr()) {
    this->state.getDiagnostics().error(returnStmt->value->loc(), match.error().message);
  }
}

void ReturnChecker::walkVoidReturn(ReturnStmt *returnStmt, const FuncSymbol *function) {
  auto empty = this->checkVoidReturn(returnStmt->value->type, function);
  if (empty.isErr()) {
    this->state.getDiagnostics().error(returnStmt->value->loc(), empty.error().message);
    this->classLinks->noteInheritedReturn(function->decl);
    return;
  }
  if (!function->discardsResult) {
    return;
  }
  auto used = this->resultChecker->checkUsedCallbackValue(returnStmt->value);
  if (used.isErr()) {
    this->state.getDiagnostics().error(returnStmt->value->loc(), used.error().message);
  }
}

void ReturnChecker::walkValueReturn(ReturnStmt *returnStmt, const FuncSymbol *function,
                                    const Type *returned) {
  auto &diagnostics = this->state.getDiagnostics();
  diagnostics.pushList(DiagnosticRoute::PARENT);
  returnStmt->value = this->typeRelations->coerce(returnStmt->value, returned);
  auto coerceDiagnostics = diagnostics.popList();
  bool isReachingProgram = diagnostics.getCurrentDestination() == DiagnosticRoute::PROGRAM;
  if (Diagnostics::hasErrors(coerceDiagnostics) && isReachingProgram) {
    this->classLinks->noteInheritedReturn(function->decl);
    return;
  }
  if (function->decl == nullptr) {
    return;
  }
  auto fits = this->byteProofs->checkByteCallbackResult(function->decl, returnStmt->value);
  if (fits.isErr()) {
    diagnostics.error(returnStmt->value->loc(), fits.error().message);
  }
}

bool ReturnChecker::isExit(const Stmt *stmt) const {
  auto *statement = dyn_cast<ExprStmt>(stmt);
  if (statement == nullptr) {
    return false;
  }
  auto *call = dyn_cast<CallExpr>(statement->expr);
  bool isStopping = call != nullptr && (call->builtin() == Builtin::ProcessExit ||
                                        call->builtin() == Builtin::LibPanic);

  return isStopping;
}

bool ReturnChecker::isAlwaysReturning(const Stmt *stmt,
                                      llvm::ArrayRef<llvm::StringRef> labels) const {
  auto whole = ReturnPart{
      .stmt = stmt,
      .labels = std::vector<llvm::StringRef>(labels.begin(), labels.end()),
  };
  auto answers = std::vector<bool>();
  for (const ReturnPart &part : ChildrenFirst::order(whole, this, &ReturnChecker::returnParts)) {
    bool isReturning = this->isNodeReturning(part, answers);
    answers.push_back(isReturning);
  }

  return answers.back();
}

NodeChildren<ReturnPart> ReturnChecker::returnParts(ReturnPart part) const {
  if (isa<LabeledStmt>(part.stmt)) {
    return {this->unlabeled(part)};
  }
  auto inner = this->innerStatements(part);
  auto parts = NodeChildren<ReturnPart>();
  for (const Stmt *stmt : inner) {
    parts.push_back(ReturnPart{.stmt = stmt});
  }

  return parts;
}

NodeChildren<const Stmt *> ReturnChecker::innerStatements(const ReturnPart &part) const {
  auto *block = dyn_cast<BlockStmt>(part.stmt);
  auto *branch = dyn_cast<IfStmt>(part.stmt);
  auto *cases = dyn_cast<SwitchStmt>(part.stmt);
  if (block != nullptr) {
    return NodeChildren<const Stmt *>(block->body.begin(), block->body.end());
  }
  if (branch != nullptr) {
    return this->ifBranches(branch);
  }
  if (cases != nullptr) {
    return this->switchTailStatements(cases, part.labels);
  }

  return {};
}

NodeChildren<const Stmt *>
ReturnChecker::switchTailStatements(const SwitchStmt *switchStmt,
                                    llvm::ArrayRef<llvm::StringRef> labels) const {
  const auto *tail = this->switchTail(switchStmt, labels);
  if (tail == nullptr) {
    return {};
  }

  return NodeChildren<const Stmt *>(tail->begin(), tail->end());
}

ReturnPart ReturnChecker::unlabeled(const ReturnPart &part) const {
  auto inner = part;
  auto *labeled = dyn_cast<LabeledStmt>(inner.stmt);
  while (labeled != nullptr) {
    inner.labels.push_back(labeled->label);
    inner.stmt = labeled->body;
    labeled = dyn_cast<LabeledStmt>(inner.stmt);
  }

  return inner;
}

NodeChildren<const Stmt *> ReturnChecker::ifBranches(const IfStmt *ifStmt) const {
  auto *literal = dyn_cast<BoolLitExpr>(ifStmt->cond);
  bool isAlwaysThen = literal != nullptr && literal->value && ifStmt->elseStmt == nullptr;
  bool isAlwaysElse = literal != nullptr && !literal->value && isa<EmptyStmt>(ifStmt->thenStmt);
  if (isAlwaysThen) {
    return {ifStmt->thenStmt};
  }
  if (isAlwaysElse) {
    return this->elseBranch(ifStmt);
  }
  if (ifStmt->elseStmt == nullptr) {
    return {};
  }

  return {ifStmt->thenStmt, ifStmt->elseStmt};
}

NodeChildren<const Stmt *> ReturnChecker::elseBranch(const IfStmt *ifStmt) const {
  if (ifStmt->elseStmt == nullptr) {
    return {};
  }

  return {ifStmt->elseStmt};
}

const std::vector<Stmt *> *ReturnChecker::switchTail(const SwitchStmt *switchStmt,
                                                     llvm::ArrayRef<llvm::StringRef> labels) const {
  bool hasDefault = switchStmt->exhaustive || this->hasDefaultCase(switchStmt);
  if (!hasDefault || switchStmt->cases.empty()) {
    return nullptr;
  }
  if (this->hasLeavingCase(switchStmt, labels)) {
    return nullptr;
  }

  return &switchStmt->cases.back().body;
}

bool ReturnChecker::hasDefaultCase(const SwitchStmt *switchStmt) const {
  for (const SwitchCase &switchCase : switchStmt->cases) {
    if (switchCase.test == nullptr) {
      return true;
    }
  }

  return false;
}

bool ReturnChecker::hasLeavingCase(const SwitchStmt *switchStmt,
                                   llvm::ArrayRef<llvm::StringRef> labels) const {
  auto exits = Exits(JumpOwner::SWITCH, labels);
  for (const SwitchCase &switchCase : switchStmt->cases) {
    if (exits.hasExitIn(switchCase.body)) {
      return true;
    }
  }

  return false;
}

bool ReturnChecker::hasLoopExit(const Stmt *body, llvm::ArrayRef<llvm::StringRef> labels) const {
  auto exits = Exits(JumpOwner::LOOP, labels);

  return exits.hasExitIn({body});
}

bool ReturnChecker::isNodeReturning(const ReturnPart &part, std::vector<bool> &answers) const {
  const Stmt *stmt = part.stmt;
  llvm::ArrayRef<llvm::StringRef> labels = part.labels;
  switch (stmt->kind()) {
    case Stmt::Kind::Return:
      return true;
    case Stmt::Kind::Expr:
      return this->isExit(stmt);
    case Stmt::Kind::Block:
      return this->isBlockReturning(cast<BlockStmt>(stmt), labels, answers);
    case Stmt::Kind::If:
      return this->isIfReturning(cast<IfStmt>(stmt), labels, answers);
    case Stmt::Kind::While:
      return this->isWhileReturning(cast<WhileStmt>(stmt), labels);
    case Stmt::Kind::DoWhile:
      return this->isDoWhileReturning(cast<DoWhileStmt>(stmt), labels);
    case Stmt::Kind::For:
      return this->isForReturning(cast<ForStmt>(stmt), labels);
    case Stmt::Kind::Switch:
      return this->isSwitchReturning(cast<SwitchStmt>(stmt), labels, answers);
    case Stmt::Kind::Labeled:
      return ChildrenFirst::takeAnswer(answers);
    case Stmt::Kind::VarDecl:
    case Stmt::Kind::Function:
    case Stmt::Kind::ForOf:
    case Stmt::Kind::Break:
    case Stmt::Kind::Continue:
    case Stmt::Kind::Empty:
    case Stmt::Kind::TypeAlias:
    case Stmt::Kind::Enum:
    case Stmt::Kind::Debugger:
    case Stmt::Kind::Class:
      return false;
  }

  return false;
}

bool ReturnChecker::isBlockReturning(const BlockStmt *block, llvm::ArrayRef<llvm::StringRef> labels,
                                     std::vector<bool> &answers) const {
  auto inner = ChildrenFirst::takeAnswers(answers, block->body.size());

  return llvm::is_contained(inner, true) && !this->isBreakingTo(block, labels);
}

bool ReturnChecker::isIfReturning(const IfStmt *ifStmt, llvm::ArrayRef<llvm::StringRef> labels,
                                  std::vector<bool> &answers) const {
  size_t count = this->ifBranches(ifStmt).size();
  auto inner = ChildrenFirst::takeAnswers(answers, count);
  bool isEveryBranch = count > 0 && !llvm::is_contained(inner, false);

  return isEveryBranch && !this->isBreakingTo(ifStmt, labels);
}

bool ReturnChecker::isWhileReturning(const WhileStmt *loop,
                                     llvm::ArrayRef<llvm::StringRef> labels) const {
  return this->isTrueLiteral(loop->cond) && !this->hasLoopExit(loop->body, labels);
}

bool ReturnChecker::isDoWhileReturning(const DoWhileStmt *loop,
                                       llvm::ArrayRef<llvm::StringRef> labels) const {
  return this->isTrueLiteral(loop->cond) && !this->hasLoopExit(loop->body, labels);
}

bool ReturnChecker::isForReturning(const ForStmt *loop,
                                   llvm::ArrayRef<llvm::StringRef> labels) const {
  bool isForever = loop->cond == nullptr || this->isTrueLiteral(loop->cond);

  return isForever && !this->hasLoopExit(loop->body, labels);
}

bool ReturnChecker::isSwitchReturning(const SwitchStmt *switchStmt,
                                      llvm::ArrayRef<llvm::StringRef> labels,
                                      std::vector<bool> &answers) const {
  const auto *tail = this->switchTail(switchStmt, labels);
  if (tail == nullptr) {
    return false;
  }
  auto inner = ChildrenFirst::takeAnswers(answers, tail->size());

  return llvm::is_contained(inner, true);
}

}
