#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/program/ast/ast_cloner/ast_cloner.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/stmt/return_checker/return_checker.h"
#include "compiler/semantic/declare/import_binder/import_binder.h"
#include "compiler/semantic/declare/module_declarer/module_declarer.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/late_classes/late_classes.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr int kMaxCycleRounds = 8;

}

ReturnInferrer::ReturnInferrer(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->classLinks = &all.classLinks;
  this->typeParams = &all.typeParams;
  this->lateClasses = &all.lateClasses;
  this->asyncChecker = &all.asyncChecker;
  this->importBinder = &all.importBinder;
  this->typeRelations = &all.typeRelations;
  this->returnChecker = &all.returnChecker;
  this->functionChecker = &all.functionChecker;
  this->generatorChecker = &all.generatorChecker;
  this->genericClassChecker = &all.genericClassChecker;
}

bool ReturnInferrer::isReturningValue(const Stmt *body) const {
  auto nodes = PlacedNodes::collect({body}, {});
  for (const PlacedStmt &placed : nodes.getStmts()) {
    auto *returnStmt = dyn_cast<ReturnStmt>(placed.stmt);
    bool isOwn = returnStmt != nullptr && !placed.place.isInsideFunction();
    if (isOwn && this->isGivingValue(returnStmt)) {
      return true;
    }
  }

  return false;
}

bool ReturnInferrer::isGivingValue(const ReturnStmt *returnStmt) const {
  if (returnStmt->value == nullptr) {
    return false;
  }
  auto *unary = dyn_cast<UnaryExpr>(returnStmt->value);
  bool isDiscarded = unary != nullptr && unary->op == UnaryOp::Void;

  return !isDiscarded;
}

ReturnInferrer::ResultNotCalledEarly ReturnInferrer::earlyCall(const FuncSymbol *function) const {
  bool isCalledWhileLoading =
      function->inferring && this->state.getReturnInference().isSuspended(function);
  if (isCalledWhileLoading) {
    auto message = ErrorTextEarlyCall::importBack(function->name);

    return err(ReturnInferrerEarlyCallError{
        .code = ReturnInferrerEarlyCallErrorCode::CALLED_WHILE_LOADING,
        .message = message,
    });
  }

  return ok(function);
}

const Type *ReturnInferrer::returnType(FuncSymbol *function) {
  if (function->hasReturnType()) {
    return function->getKnownReturnType();
  }
  if (this->lateClasses->isTemplate(function)) {
    return Type::unknown();
  }
  auto early = this->earlyCall(function);
  if (early.isErr()) {
    this->state.getDiagnostics().error(function->loc, early.error().message);
    this->state.getReturnInference().forgetSuspended(function);

    return Type::error();
  }
  if (function->inferring) {
    return function->assumed != nullptr ? function->assumed : Type::error();
  }
  this->inferReturnType(function);

  return function->getKnownReturnType();
}

const Type *ReturnInferrer::functionType(FuncSymbol *function) {
  const auto *returnType = this->returnType(function);
  const Type *returned = returnType != nullptr ? returnType : Type::error();

  return Type::function(function->params, function->minArgs, function->hasRest, returned,
                        function->hasThis);
}

BodyState ReturnInferrer::enterBody(const FunctionDecl *functionDecl) {
  auto saved = BodyState{
      .flow = this->state.getFlowState().takeFlow(),
      .scopeStack = this->state.getScopeStack().saveBody(),
      .functionStack = this->state.getFunctionStack().saveBody(),
      .blockPosition = this->state.getBlockPosition().saveBody(),
      .module = this->state.getModuleStates().getCurrentModule(),
      .instance = this->state.getGenericStore().getCheckingInstance(),
  };
  NestedGroup *group = functionDecl != nullptr ? functionDecl->group : nullptr;
  if (group != nullptr) {
    this->state.getScopeStack().keepGroupScopes(group);
    this->state.getFunctionStack().keepGroupFunctions(group);
  } else if (functionDecl == nullptr || !functionDecl->isExpression()) {
    this->enterTopLevelBody(functionDecl);
  }
  this->state.getScopeStack().alignBranchesWithScopes();

  return saved;
}

void ReturnInferrer::enterTopLevelBody(const FunctionDecl *functionDecl) {
  this->state.getScopeStack().keepTopScopes();
  this->state.getFunctionStack().forgetOuterFunctions();
  this->state.getBlockPosition().forgetOtherCaseVars();
  if (functionDecl != nullptr) {
    this->importBinder->enterHomeModule(functionDecl);
  }
  this->state.getGenericStore().forgetCheckingInstance();
  auto *owner = this->lateOwnerOf(functionDecl);
  if (owner != nullptr) {
    this->state.getGenericStore().rememberCheckingInstance(owner);
  }
}

InstanceRecord *ReturnInferrer::lateOwnerOf(const FunctionDecl *functionDecl) const {
  if (functionDecl == nullptr || functionDecl->cls() == nullptr) {
    return nullptr;
  }

  return this->state.getLateClassStore().findLateOwner(functionDecl->cls());
}

void ReturnInferrer::leaveBody(BodyState &saved) {
  this->state.getFlowState().replaceFlow(std::move(saved.flow));
  this->state.getScopeStack().restoreBody(std::move(saved.scopeStack));
  this->state.getFunctionStack().restoreBody(std::move(saved.functionStack));
  this->state.getBlockPosition().restoreBody(std::move(saved.blockPosition));
  this->state.getModuleStates().rememberCurrentModule(saved.module);
  this->state.getGenericStore().rememberCheckingInstance(saved.instance);
}

const Type *ReturnInferrer::templateReturn(FuncSymbol *function) {
  FunctionDecl *functionDecl = function->decl;
  FuncSymbol *generic = functionDecl->generic->sym;
  const auto *returnType = this->returnType(generic);
  if (returnType == nullptr) {
    return Type::error();
  }
  const auto *instance = this->genericClassChecker->instanceType(functionDecl->cls(), returnType);

  return this->typeParams->substitute(instance, generic->typeParams, function->typeParams);
}

bool ReturnInferrer::isTemplateCopy(const FunctionDecl *functionDecl) {
  if (functionDecl == nullptr || functionDecl->generic == nullptr) {
    return false;
  }
  const ClassDecl *classDecl = functionDecl->cls();
  if (classDecl == nullptr || classDecl->generic() == nullptr) {
    return false;
  }

  return !this->lateClasses->hasClassExpr(functionDecl->generic);
}

void ReturnInferrer::inferReturnType(FuncSymbol *function) {
  FunctionDecl *functionDecl = function->decl;
  if (this->isTemplateCopy(functionDecl)) {
    const auto *fromTemplate = this->templateReturn(function);
    function->setReturnType(fromTemplate);
    return;
  }
  const auto *inherited = this->classLinks->inheritedReturnType(function);
  if (inherited != nullptr) {
    function->setReturnType(inherited);
    return;
  }
  auto check = this->startBodyCheck(function);
  auto &inference = this->state.getReturnInference();
  if (inference.isInCycle(function)) {
    auto cycle = inference.getCycleOf(function);
    this->inferCycle(cycle);
  } else {
    this->inferFromBody(functionDecl);
  }
  this->finishBodyCheck(check);
}

void ReturnInferrer::walkBodyNow(FunctionDecl *functionDecl) {
  if (!functionDecl->sym->hasReturnType()) {
    this->inferReturnType(functionDecl->sym);
  }
  if (functionDecl->checked) {
    return;
  }
  auto check = this->startBodyCheck(functionDecl->sym);
  this->functionChecker->walkFunctionFromScratch(functionDecl);
  this->finishBodyCheck(check);
}

BodyCheck ReturnInferrer::startBodyCheck(FuncSymbol *function) {
  FunctionDecl *functionDecl = function->decl;
  auto saved = this->enterBody(functionDecl);
  bool isClosure = functionDecl->isClosure();
  InstanceRecord *lateOwner = isClosure ? nullptr : this->lateOwnerOf(functionDecl);
  auto route = this->bodyRoute(isClosure, lateOwner);
  this->state.getDiagnostics().pushList(route);
  bool isTopLevelEarly = !isClosure && !this->state.getModuleStates().isTopLevelDone();
  bool isGroupEarly = functionDecl->group != nullptr && !functionDecl->group->finished;
  if (isTopLevelEarly || isGroupEarly) {
    this->state.getFunctionStack().rememberEarlyFunction(function);
  }

  return BodyCheck{.saved = std::move(saved), .lateOwner = lateOwner};
}

DiagnosticRoute ReturnInferrer::bodyRoute(bool isClosure, const InstanceRecord *lateOwner) const {
  if (isClosure) {
    return DiagnosticRoute::PARENT;
  }
  if (lateOwner != nullptr) {
    return DiagnosticRoute::OWNER;
  }

  return DiagnosticRoute::PROGRAM;
}

void ReturnInferrer::finishBodyCheck(BodyCheck &check) {
  auto bodyDiagnostics = this->state.getDiagnostics().popList();
  if (check.lateOwner != nullptr) {
    this->state.getLateClassStore().addLateErrors(check.lateOwner, bodyDiagnostics);
  }
  this->leaveBody(check.saved);
}

void ReturnInferrer::inferFromBody(FunctionDecl *functionDecl) {
  FuncSymbol *function = functionDecl->sym;
  auto collected = CollectedReturns{.returns = {}, .yields = {}};
  auto &functions = this->state.getFunctionStack();
  auto *outerYields = functions.getYieldCollector();
  auto *outerReturns = functions.getReturnCollector();
  functions.rememberReturnCollector(&collected.returns);
  functions.rememberYieldCollector(&collected.yields);
  function->inferring = true;
  this->state.getReturnInference().enterInference(function);
  this->functionChecker->walkFunctionBody(functionDecl);
  this->state.getReturnInference().leaveInference();
  function->inferring = false;
  functions.rememberReturnCollector(outerReturns);
  functions.rememberYieldCollector(outerYields);
  bool isFallingOff = !this->returnChecker->alwaysReturns(functionDecl->body);
  const auto *type =
      this->collectedType(functionDecl, function, collected, isFallingOff, ReturnErrors::REPORTED);
  function->setReturnType(type != nullptr ? type : Type::error());
  if (functionDecl->isAsync && !functionDecl->isGenerator) {
    return this->asyncChecker->finishAsyncReturns(function, collected.returns);
  }

  return this->convertReturns(functionDecl, collected.returns, collected.yields);
}

const Type *ReturnInferrer::collectedType(const FunctionDecl *functionDecl,
                                          const FuncSymbol *function,
                                          const CollectedReturns &collected, bool isFallingOff,
                                          ReturnErrors errors) {
  if (functionDecl->isGenerator) {
    return this->generatorChecker->inferGeneratorType(function, collected.yields, collected.returns,
                                                      isFallingOff, errors);
  }
  if (functionDecl->isAsync) {
    return this->asyncChecker->inferAsyncType(function, collected.returns, isFallingOff, errors);
  }

  return this->unifyReturns(function, collected.returns, isFallingOff, errors);
}

void ReturnInferrer::convertReturns(const FunctionDecl *functionDecl,
                                    llvm::ArrayRef<ReturnStmt *> returns,
                                    llvm::ArrayRef<YieldExpr *> yields) {
  const Type *known = functionDecl->sym->getKnownReturnType();
  bool isAsync = functionDecl->isAsync;
  bool isIterating = known->isIterator() || known->isAsyncIterator();
  const Type *returned = functionDecl->isGenerator && isIterating ? known->returned() : known;
  if (!returned->isError() && !returned->isVoid()) {
    for (ReturnStmt *returnStmt : returns) {
      if (this->isConvertibleReturn(returnStmt, isAsync)) {
        returnStmt->value = this->typeRelations->convert(returnStmt->value, returned);
      }
    }
  }
  if (!isIterating) {
    return;
  }
  const Type *yielded = known->yielded();
  for (YieldExpr *yield : yields) {
    if (this->isConvertibleYield(yield, isAsync)) {
      yield->value = this->typeRelations->convert(yield->value, yielded);
    }
  }
}

bool ReturnInferrer::isSettled(const Type *type) const {
  const auto *awaited = AsyncChecker::awaitedType(type);

  return awaited == type;
}

bool ReturnInferrer::isConvertibleReturn(const ReturnStmt *returnStmt, bool isAsync) const {
  const Expr *value = returnStmt->value;
  if (value == nullptr || value->type->isError()) {
    return false;
  }
  if (!isAsync) {
    return true;
  }

  return this->isSettled(value->type) && !value->type->isUndefined();
}

bool ReturnInferrer::isConvertibleYield(const YieldExpr *yield, bool isAsync) const {
  if (yield->delegate || yield->value == nullptr || yield->value->type->isError()) {
    return false;
  }
  if (!isAsync) {
    return true;
  }

  return this->isSettled(yield->value->type);
}

const Type *ReturnInferrer::trialReturn(FuncSymbol *function) {
  auto *copy = this->freshCopy(function->decl);
  auto collected = CollectedReturns{.returns = {}, .yields = {}};
  auto &functions = this->state.getFunctionStack();
  functions.rememberReturnCollector(&collected.returns);
  functions.rememberYieldCollector(&collected.yields);
  functions.enterTrial();
  this->functionChecker->walkFunctionFromScratch(copy);
  functions.leaveTrial();
  functions.rememberReturnCollector(nullptr);
  functions.rememberYieldCollector(nullptr);
  bool isFallingOff = !this->returnChecker->alwaysReturns(copy->body);

  return this->collectedType(function->decl, function, collected, isFallingOff,
                             ReturnErrors::IGNORED);
}

ReturnInferrer::ResultCycleTypeKnown ReturnInferrer::cycleType(const FuncSymbol *function,
                                                               int cycleSize, bool isFailed) const {
  bool isUnknown = function->getKnownReturnType()->isError();
  if (isUnknown && isFailed) {
    auto message = ErrorTextCycleType::differ(function->name);

    return err(ReturnInferrerCycleTypeError{
        .code = ReturnInferrerCycleTypeErrorCode::DIFFERENT_TYPES,
        .message = message,
    });
  }
  if (isUnknown && cycleSize == 1) {
    auto message = ErrorTextCycleType::selfOnly(function->name);

    return err(ReturnInferrerCycleTypeError{
        .code = ReturnInferrerCycleTypeErrorCode::ONLY_SELF_CALLS,
        .message = message,
    });
  }
  if (isUnknown) {
    auto message = ErrorTextCycleType::calledBack(function->name);

    return err(ReturnInferrerCycleTypeError{
        .code = ReturnInferrerCycleTypeErrorCode::ONLY_CALLED_BACK,
        .message = message,
    });
  }

  return ok(function->getKnownReturnType());
}

void ReturnInferrer::inferCycle(llvm::ArrayRef<FuncSymbol *> cycle) {
  for (FuncSymbol *function : cycle) {
    function->inferring = true;
  }
  for (FuncSymbol *function : cycle) {
    this->state.getReturnInference().enterInference(function);
  }
  this->state.getDiagnostics().pushList(DiagnosticRoute::DISCARD);
  auto settled = this->settleCycle(cycle);
  this->state.getDiagnostics().popList();
  for (size_t index = 0; index < cycle.size(); ++index) {
    this->state.getReturnInference().leaveInference();
  }
  bool isFailed = settled == CycleStep::FAILED;
  for (FuncSymbol *function : cycle) {
    function->inferring = false;
    bool isAssumed = function->assumed != nullptr && !isFailed;
    function->setReturnType(isAssumed ? function->assumed : Type::error());
    function->assumed = nullptr;
  }
  this->reportCycleTypes(cycle, isFailed);
}

ReturnInferrer::CycleStep ReturnInferrer::settleCycle(llvm::ArrayRef<FuncSymbol *> cycle) {
  for (int round = 0; round < kMaxCycleRounds; ++round) {
    auto step = this->cycleRound(cycle);
    if (step != CycleStep::CHANGED) {
      return step;
    }
  }

  return CycleStep::CHANGED;
}

ReturnInferrer::CycleStep ReturnInferrer::cycleRound(llvm::ArrayRef<FuncSymbol *> cycle) {
  auto round = CycleStep::KEPT;
  for (FuncSymbol *function : cycle) {
    auto step = this->nextAssumption(function);
    if (step == CycleStep::FAILED) {
      return step;
    }
    if (step == CycleStep::CHANGED) {
      round = step;
    }
  }

  return round;
}

ReturnInferrer::CycleStep ReturnInferrer::nextAssumption(FuncSymbol *function) {
  const auto *found = this->trialReturn(function);
  if (found == nullptr) {
    return CycleStep::KEPT;
  }
  const Type *assumed = function->assumed;
  const Type *next = assumed != nullptr ? LiteralTypes::unionUnify(assumed, found) : found;
  if (next == nullptr || next->isError()) {
    return CycleStep::FAILED;
  }
  if (next == assumed) {
    return CycleStep::KEPT;
  }
  function->assumed = next;

  return CycleStep::CHANGED;
}

void ReturnInferrer::reportCycleTypes(llvm::ArrayRef<FuncSymbol *> cycle, bool isFailed) {
  int cycleSize = int(cycle.size());
  for (FuncSymbol *function : cycle) {
    auto known = this->cycleType(function, cycleSize, isFailed);
    if (known.isErr()) {
      this->state.getDiagnostics().error(function->loc, known.error().message);
    }
  }
}

bool ReturnInferrer::isIntLiteralReturn(const ReturnStmt *returnStmt) const {
  const Expr *value = returnStmt->value;
  if (value == nullptr || value->type == nullptr || !value->type->isInteger()) {
    return false;
  }

  return this->typeRelations->isIntLiteralTree(value);
}

std::vector<ReturnStmt *> ReturnInferrer::literalsLast(llvm::ArrayRef<ReturnStmt *> returns) const {
  auto ordered = std::vector<ReturnStmt *>();
  auto literals = std::vector<ReturnStmt *>();
  for (ReturnStmt *returnStmt : returns) {
    if (this->isIntLiteralReturn(returnStmt)) {
      literals.push_back(returnStmt);
    } else {
      ordered.push_back(returnStmt);
    }
  }
  ordered.insert(ordered.end(), literals.begin(), literals.end());

  return ordered;
}

ReturnInferrer::ResultReturnsUnified
ReturnInferrer::commonReturn(const FuncSymbol *function, const Type *before,
                             const ReturnStmt *returnStmt) const {
  const Type *found = returnStmt->value->type;
  if (before == nullptr) {
    return ok(found);
  }
  bool isAdapting =
      before != found && this->typeRelations->isLiteralAdapting(returnStmt->value, before);
  if (isAdapting) {
    return ok(before);
  }
  const auto *common = LiteralTypes::unionUnify(before, found);
  if (common == nullptr || common->isError()) {
    auto message = ErrorTextCommonReturn::differ(function->name, found->name(), before->name());

    return err(ReturnInferrerCommonReturnError{
        .code = ReturnInferrerCommonReturnErrorCode::DIFFERENT_TYPES,
        .message = message,
    });
  }

  return ok(common);
}

bool ReturnInferrer::hasValuedReturn(llvm::ArrayRef<ReturnStmt *> returns) const {
  for (const ReturnStmt *returnStmt : returns) {
    if (returnStmt->value != nullptr) {
      return true;
    }
  }

  return false;
}

ReturnInferrer::ResultReturnsDefined
ReturnInferrer::definedReturn(const FuncSymbol *function, llvm::ArrayRef<ReturnStmt *> returns,
                              bool isUndefined) const {
  if (isUndefined && this->hasValuedReturn(returns)) {
    auto message = ErrorTextDefinedReturn::undefined(function->name);

    return err(ReturnInferrerDefinedReturnError{
        .code = ReturnInferrerDefinedReturnErrorCode::ONLY_UNDEFINED,
        .message = message,
    });
  }

  return ok(function);
}

bool ReturnInferrer::isUndefinedReturn(const ReturnStmt *returnStmt) const {
  return returnStmt->value == nullptr || returnStmt->value->type->isUndefined();
}

bool ReturnInferrer::hasUndefinedReturn(llvm::ArrayRef<ReturnStmt *> returns) const {
  for (const ReturnStmt *returnStmt : returns) {
    if (this->isUndefinedReturn(returnStmt)) {
      return true;
    }
  }

  return false;
}

std::vector<const ReturnStmt *>
ReturnInferrer::typedReturns(llvm::ArrayRef<ReturnStmt *> returns) const {
  auto typed = std::vector<const ReturnStmt *>();
  for (const ReturnStmt *returnStmt : this->literalsLast(returns)) {
    bool isTyped = !this->isUndefinedReturn(returnStmt) && !returnStmt->value->type->isError();
    if (isTyped) {
      typed.push_back(returnStmt);
    }
  }

  return typed;
}

const Type *ReturnInferrer::unifyReturns(const FuncSymbol *function,
                                         llvm::ArrayRef<ReturnStmt *> returns, bool isFallingOff,
                                         ReturnErrors errors) {
  const Type *type = nullptr;
  for (const ReturnStmt *returnStmt : this->typedReturns(returns)) {
    auto common = this->commonReturn(function, type, returnStmt);
    if (common.isErr() && errors == ReturnErrors::REPORTED) {
      this->state.getDiagnostics().error(returnStmt->value->loc(), common.error().message);
    }
    if (common.isErr()) {
      return Type::error();
    }
    type = common.value();
  }
  bool isUndefined = isFallingOff || this->hasUndefinedReturn(returns);
  if (type != nullptr && isUndefined && !type->isVoid()) {
    return Type::optional(type);
  }
  if (type != nullptr) {
    return type;
  }
  auto defined = this->definedReturn(function, returns, isUndefined);
  if (defined.isErr() && errors == ReturnErrors::REPORTED) {
    this->state.getDiagnostics().error(function->loc, defined.error().message);
  }

  return errors == ReturnErrors::REPORTED ? Type::error() : nullptr;
}

FunctionDecl *ReturnInferrer::freshCopy(const FunctionDecl *functionDecl) {
  AstCloner cloner(this->state.getAst());
  FunctionDecl *copy = cloner.copyFunction(functionDecl);
  copy->place = functionDecl->place;
  copy->isAsync = functionDecl->isAsync;
  this->copyMemberRole(functionDecl, copy);
  copy->sym = functionDecl->sym;
  copy->name = functionDecl->name;
  copy->typeArgs = functionDecl->typeArgs;
  copy->isInstance = functionDecl->isInstance;
  copy->parent = functionDecl->parent;
  copy->group = functionDecl->group;
  copy->var = functionDecl->var;
  copy->creationSeq = functionDecl->creationSeq;
  this->copyParams(functionDecl, copy);

  return copy;
}

void ReturnInferrer::copyMemberRole(const FunctionDecl *functionDecl, FunctionDecl *copy) const {
  const ClassMember *original = functionDecl->member();
  if (original == nullptr) {
    return;
  }
  auto copied = ClassMember{.cls = original->cls, .access = original->access};
  if (functionDecl->isStatic()) {
    copied.method = StaticMethod{};
  } else if (functionDecl->isConstructor()) {
    copied.method = Constructor{};
  }
  copy->role = std::move(copied);
}

void ReturnInferrer::copyParams(const FunctionDecl *functionDecl, FunctionDecl *copy) const {
  bool hasList = !functionDecl->listName.empty() && copy->params.size() > functionDecl->listStart;
  if (hasList) {
    copy->params.resize(functionDecl->listStart);
    copy->listName = functionDecl->listName;
    copy->listStart = functionDecl->listStart;
    copy->listType = functionDecl->listType;
  }
  size_t shared = std::min(copy->params.size(), functionDecl->params.size());
  for (size_t index = 0; index < shared; ++index) {
    copy->params[index].declaredType = functionDecl->params[index].declaredType;
  }
  for (size_t index = copy->params.size(); index < functionDecl->params.size(); ++index) {
    copy->params.push_back(functionDecl->params[index]);
  }
}

}
