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

ReturnInferrer::ReturnInferrer(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->classLinks = &all.classLinks;
  this->typeParams = &all.typeParams;
  this->lateClasses = &all.lateClasses;
  this->asyncChecker = &all.asyncChecker;
  this->importBinder = &all.importBinder;
  this->typeRelations = &all.typeRelations;
  this->functionChecker = &all.functionChecker;
  this->generatorChecker = &all.generatorChecker;
  this->genericClassChecker = &all.genericClassChecker;
}

bool ReturnInferrer::returnsValue(const Stmt *body) {
  PlacedNodes nodes = PlacedNodes::collect({body}, {});
  for (const PlacedStmt &placed : nodes.getStmts()) {
    if (placed.place.isInsideFunction()) {
      continue;
    }
    auto *result = dyn_cast<ReturnStmt>(placed.stmt);
    if (result != nullptr && ReturnInferrer::givesValue(result)) {
      return true;
    }
  }

  return false;
}

bool ReturnInferrer::givesValue(const ReturnStmt *result) {
  if (result->value == nullptr) {
    return false;
  }
  auto *unary = dyn_cast<UnaryExpr>(result->value);
  bool isDiscarded = unary != nullptr && unary->op == UnaryOp::Void;

  return !isDiscarded;
}

ReturnInferrer::ResultNotCalledEarly ReturnInferrer::earlyCall(const FuncSymbol *fn) const {
  bool isCalledWhileLoading = fn->inferring && this->state.getReturnInference().isSuspended(fn);
  if (isCalledWhileLoading) {
    std::string message = ErrorTextEarlyCall::importBack(fn->name);

    return err(ReturnInferrerEarlyCallError{
        .code = ReturnInferrerEarlyCallErrorCode::CALLED_WHILE_LOADING,
        .message = message,
    });
  }

  return ok(fn);
}

const Type *ReturnInferrer::returnType(FuncSymbol *fn) {
  if (fn->hasReturnType()) {
    return fn->getKnownReturnType();
  }
  if (this->lateClasses->isTemplate(fn)) {
    return Type::unknown();
  }
  ResultNotCalledEarly early = this->earlyCall(fn);
  if (early.isErr()) {
    this->state.getDiagnostics().error(fn->loc, early.error().message);
    this->state.getReturnInference().forgetSuspended(fn);
    return Type::error();
  }
  if (fn->inferring) {
    return fn->assumed ? fn->assumed : Type::error();
  }
  this->inferReturnType(fn);

  return fn->getKnownReturnType();
}

const Type *ReturnInferrer::functionType(FuncSymbol *fn) {
  const Type *ret = this->returnType(fn);
  const Type *returned = ret ? ret : Type::error();

  return Type::function(fn->params, fn->minArgs, fn->hasRest, returned, fn->hasThis);
}

BodyState ReturnInferrer::enterBody(const FunctionDecl *fn) {
  BodyState saved;
  saved.flow = this->state.getFlowState().takeFlow();
  saved.scopeStack = this->state.getScopeStack().saveBody();
  saved.functionStack = this->state.getFunctionStack().saveBody();
  saved.blockPosition = this->state.getBlockPosition().saveBody();
  saved.module = this->state.getModuleStates().getCurrentModule();
  saved.instance = this->state.getGenericStore().getCheckingInstance();
  NestedGroup *group = fn ? fn->group : nullptr;
  if (group) {
    this->state.getScopeStack().keepGroupScopes(group);
    this->state.getFunctionStack().keepGroupFunctions(group);
  } else if (!fn || !fn->isExpression()) {
    this->state.getScopeStack().keepTopScopes();
    this->state.getFunctionStack().forgetOuterFunctions();
    this->state.getBlockPosition().forgetOtherCaseVars();
    Module *home = fn ? this->state.getModuleStates().findDeclModule(fn) : nullptr;
    if (home) {
      this->importBinder->enterModule(home);
    }
    this->state.getGenericStore().forgetCheckingInstance();
    InstanceRecord *owner =
        fn && fn->cls() ? this->state.getLateClassStore().findLateOwner(fn->cls()) : nullptr;
    if (owner) {
      this->state.getGenericStore().rememberCheckingInstance(owner);
    }
  }
  this->state.getScopeStack().alignBranchesWithScopes();

  return saved;
}

void ReturnInferrer::leaveBody(BodyState &saved) {
  this->state.getFlowState().replaceFlow(std::move(saved.flow));
  this->state.getScopeStack().restoreBody(std::move(saved.scopeStack));
  this->state.getFunctionStack().restoreBody(std::move(saved.functionStack));
  this->state.getBlockPosition().restoreBody(std::move(saved.blockPosition));
  this->state.getModuleStates().rememberCurrentModule(saved.module);
  this->state.getGenericStore().rememberCheckingInstance(saved.instance);
}

const Type *ReturnInferrer::templateReturn(FuncSymbol *fn) {
  FunctionDecl *decl = fn->decl;
  FuncSymbol *generic = decl->generic->sym;
  const Type *ret = this->returnType(generic);
  if (!ret) {
    return Type::error();
  }
  const Type *instance = this->genericClassChecker->instanceType(decl->cls(), ret);

  return this->typeParams->substitute(instance, generic->typeParams, fn->typeParams);
}

void ReturnInferrer::inferReturnType(FuncSymbol *fn) {
  FunctionDecl *decl = fn->decl;
  bool isFromTemplate = decl && decl->generic && decl->cls() && decl->cls()->generic() &&
                        !this->lateClasses->hasClassExpr(decl->generic);
  if (isFromTemplate) {
    fn->setReturnType(this->templateReturn(fn));
    return;
  }
  const Type *inherited = this->classLinks->inheritedReturnType(fn);
  if (inherited) {
    fn->setReturnType(inherited);
    return;
  }

  auto check = this->startBodyCheck(fn);
  ReturnInference &inference = this->state.getReturnInference();
  if (inference.isInCycle(fn)) {
    this->inferCycle(inference.getCycleOf(fn));
  } else {
    this->inferFromBody(decl);
  }
  this->finishBodyCheck(check);
}

void ReturnInferrer::walkBodyNow(FunctionDecl *decl) {
  if (!decl->sym->hasReturnType()) {
    this->inferReturnType(decl->sym);
  }
  if (decl->checked) {
    return;
  }
  auto check = this->startBodyCheck(decl->sym);
  this->functionChecker->walkFunctionFromScratch(decl);
  this->finishBodyCheck(check);
}

BodyCheck ReturnInferrer::startBodyCheck(FuncSymbol *fn) {
  FunctionDecl *decl = fn->decl;
  BodyState saved = this->enterBody(decl);
  bool isClosure = decl->isClosure();
  InstanceRecord *lateOwner = nullptr;
  if (!isClosure && decl->cls() != nullptr) {
    lateOwner = this->state.getLateClassStore().findLateOwner(decl->cls());
  }
  DiagnosticRoute route = DiagnosticRoute::PROGRAM;
  if (isClosure) {
    route = DiagnosticRoute::PARENT;
  } else if (lateOwner != nullptr) {
    route = DiagnosticRoute::OWNER;
  }
  this->state.getDiagnostics().pushList(route);
  bool isTopLevelEarly = !isClosure && !this->state.getModuleStates().isTopLevelDone();
  bool isGroupEarly = decl->group != nullptr && !decl->group->finished;
  if (isTopLevelEarly || isGroupEarly) {
    this->state.getFunctionStack().rememberEarlyFunction(fn);
  }

  return BodyCheck{.saved = std::move(saved), .lateOwner = lateOwner};
}

void ReturnInferrer::finishBodyCheck(BodyCheck &check) {
  auto bodyDiagnostics = this->state.getDiagnostics().popList();
  if (check.lateOwner != nullptr) {
    this->state.getLateClassStore().addLateErrors(check.lateOwner, bodyDiagnostics);
  }
  this->leaveBody(check.saved);
}

void ReturnInferrer::inferFromBody(FunctionDecl *decl) {
  FuncSymbol *fn = decl->sym;
  std::vector<ReturnStmt *> returns;
  std::vector<YieldExpr *> yields;
  FunctionStack &functions = this->state.getFunctionStack();
  std::vector<YieldExpr *> *outerYields = functions.getYieldCollector();
  std::vector<ReturnStmt *> *outerReturns = functions.getReturnCollector();
  functions.rememberReturnCollector(&returns);
  functions.rememberYieldCollector(&yields);
  fn->inferring = true;
  this->state.getReturnInference().enterInference(fn);
  this->functionChecker->walkFunctionBody(decl);
  this->state.getReturnInference().leaveInference();
  fn->inferring = false;
  functions.rememberReturnCollector(outerReturns);
  functions.rememberYieldCollector(outerYields);

  bool isFallingOff = !ReturnChecker::alwaysReturns(decl->body);
  const Type *type = nullptr;
  if (decl->isGenerator) {
    type = this->generatorChecker->inferGeneratorType(fn, yields, returns, isFallingOff, true);
  } else if (decl->isAsync) {
    type = this->asyncChecker->inferAsyncType(fn, returns, isFallingOff, true);
  } else {
    type = this->unifyReturns(fn, returns, isFallingOff, true);
  }
  fn->setReturnType(type ? type : Type::error());
  if (decl->isAsync && !decl->isGenerator) {
    this->asyncChecker->finishAsyncReturns(fn, returns);
    return;
  }
  this->convertReturns(decl, returns, yields);
}

void ReturnInferrer::convertReturns(const FunctionDecl *decl, llvm::ArrayRef<ReturnStmt *> returns,
                                    llvm::ArrayRef<YieldExpr *> yields) {
  FuncSymbol *fn = decl->sym;
  bool isAsync = decl->isAsync;
  bool isIterating =
      fn->getKnownReturnType()->isIterator() || fn->getKnownReturnType()->isAsyncIterator();
  const Type *returned = decl->isGenerator && isIterating ? fn->getKnownReturnType()->returned()
                                                          : fn->getKnownReturnType();
  if (!returned->isError() && !returned->isVoid()) {
    for (ReturnStmt *ret : returns) {
      bool isTyped = ret->value && !ret->value->type->isError();
      bool isSettled = isTyped && isAsync &&
                       AsyncChecker::awaitedType(ret->value->type) == ret->value->type &&
                       !ret->value->type->isUndefined();
      if (isTyped && (!isAsync || isSettled)) {
        ret->value = this->typeRelations->convert(ret->value, returned);
      }
    }
  }
  if (isIterating) {
    const Type *yielded = fn->getKnownReturnType()->yielded();
    for (YieldExpr *yield : yields) {
      bool isTyped = !yield->delegate && yield->value && !yield->value->type->isError();
      bool isSettled =
          isTyped && isAsync && AsyncChecker::awaitedType(yield->value->type) == yield->value->type;
      if (isTyped && (!isAsync || isSettled)) {
        yield->value = this->typeRelations->convert(yield->value, yielded);
      }
    }
  }
}

const Type *ReturnInferrer::trialReturn(FuncSymbol *fn) {
  FunctionDecl *copy = this->freshCopy(fn->decl);
  std::vector<ReturnStmt *> returns;
  std::vector<YieldExpr *> yields;
  FunctionStack &functions = this->state.getFunctionStack();
  functions.rememberReturnCollector(&returns);
  functions.rememberYieldCollector(&yields);
  functions.enterTrial();
  this->functionChecker->walkFunctionFromScratch(copy);
  functions.leaveTrial();
  functions.rememberReturnCollector(nullptr);
  functions.rememberYieldCollector(nullptr);

  bool isFallingOff = !ReturnChecker::alwaysReturns(copy->body);
  const Type *found = nullptr;
  if (fn->decl->isGenerator) {
    found = this->generatorChecker->inferGeneratorType(fn, yields, returns, isFallingOff, false);
  } else if (fn->decl->isAsync) {
    found = this->asyncChecker->inferAsyncType(fn, returns, isFallingOff, false);
  } else {
    found = this->unifyReturns(fn, returns, isFallingOff, false);
  }

  return found;
}

ReturnInferrer::ResultCycleTypeKnown ReturnInferrer::cycleType(const FuncSymbol *fn, int cycleSize,
                                                               bool failed) const {
  bool isUnknown = fn->getKnownReturnType()->isError();
  if (isUnknown && failed) {
    std::string message = ErrorTextCycleType::differ(fn->name);

    return err(ReturnInferrerCycleTypeError{
        .code = ReturnInferrerCycleTypeErrorCode::DIFFERENT_TYPES,
        .message = message,
    });
  }
  if (isUnknown && cycleSize == 1) {
    std::string message = ErrorTextCycleType::selfOnly(fn->name);

    return err(ReturnInferrerCycleTypeError{
        .code = ReturnInferrerCycleTypeErrorCode::ONLY_SELF_CALLS,
        .message = message,
    });
  }
  if (isUnknown) {
    std::string message = ErrorTextCycleType::calledBack(fn->name);

    return err(ReturnInferrerCycleTypeError{
        .code = ReturnInferrerCycleTypeErrorCode::ONLY_CALLED_BACK,
        .message = message,
    });
  }

  return ok(fn->getKnownReturnType());
}

void ReturnInferrer::inferCycle(llvm::ArrayRef<FuncSymbol *> cycle) {
  for (FuncSymbol *fn : cycle) {
    fn->inferring = true;
  }
  for (FuncSymbol *fn : cycle) {
    this->state.getReturnInference().enterInference(fn);
  }
  this->state.getDiagnostics().pushList(DiagnosticRoute::DISCARD);
  bool isFailed = false;
  for (int round = 0; round < 8 && !isFailed; ++round) {
    bool hasChanged = false;
    for (FuncSymbol *fn : cycle) {
      const Type *found = this->trialReturn(fn);
      if (!found) {
        continue;
      }
      const Type *next = fn->assumed ? LiteralTypes::unionUnify(fn->assumed, found) : found;
      if (!next || next->isError()) {
        isFailed = true;
        break;
      }
      if (next != fn->assumed) {
        fn->assumed = next;
        hasChanged = true;
      }
    }
    if (!hasChanged) {
      break;
    }
  }
  this->state.getDiagnostics().popList();
  for (size_t i = 0; i < cycle.size(); ++i) {
    this->state.getReturnInference().leaveInference();
  }

  for (FuncSymbol *fn : cycle) {
    fn->inferring = false;
    fn->setReturnType(fn->assumed && !isFailed ? fn->assumed : Type::error());
    fn->assumed = nullptr;
  }
  int cycleSize = int(cycle.size());
  for (FuncSymbol *fn : cycle) {
    ResultCycleTypeKnown known = this->cycleType(fn, cycleSize, isFailed);
    if (known.isErr()) {
      this->state.getDiagnostics().error(fn->loc, known.error().message);
    }
  }
}

std::vector<ReturnStmt *> ReturnInferrer::literalsLast(llvm::ArrayRef<ReturnStmt *> returns) {
  std::vector<ReturnStmt *> ordered;
  std::vector<ReturnStmt *> literals;
  for (ReturnStmt *ret : returns) {
    bool isLiteral = ret->value && ret->value->type && ret->value->type->isInteger() &&
                     TypeRelations::isIntLiteralTree(ret->value);
    if (isLiteral) {
      literals.push_back(ret);
    } else {
      ordered.push_back(ret);
    }
  }
  ordered.insert(ordered.end(), literals.begin(), literals.end());

  return ordered;
}

ReturnInferrer::ResultReturnsUnified ReturnInferrer::commonReturn(const FuncSymbol *fn,
                                                                  const Type *before,
                                                                  const ReturnStmt *ret) const {
  const Type *found = ret->value->type;
  const Type *common = LiteralTypes::unionUnify(before, found);
  if (!common || common->isError()) {
    std::string message = ErrorTextCommonReturn::differ(fn->name, found->name(), before->name());

    return err(ReturnInferrerCommonReturnError{
        .code = ReturnInferrerCommonReturnErrorCode::DIFFERENT_TYPES,
        .message = message,
    });
  }

  return ok(common);
}

ReturnInferrer::ResultReturnsDefined
ReturnInferrer::definedReturn(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                              bool undefined) const {
  auto withValue = [](const ReturnStmt *ret) { return ret->value != nullptr; };
  bool isOnlyUndefined = undefined && llvm::any_of(returns, withValue);
  if (isOnlyUndefined) {
    std::string message = ErrorTextDefinedReturn::undefined(fn->name);

    return err(ReturnInferrerDefinedReturnError{
        .code = ReturnInferrerDefinedReturnErrorCode::ONLY_UNDEFINED,
        .message = message,
    });
  }

  return ok(fn);
}

const Type *ReturnInferrer::unifyReturns(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                                         bool fallsOff, bool report) {
  const Type *type = nullptr;
  bool isUndefined = fallsOff;
  std::vector<ReturnStmt *> ordered = ReturnInferrer::literalsLast(returns);
  for (ReturnStmt *ret : ordered) {
    if (!ret->value) {
      isUndefined = true;
      continue;
    }
    const Type *found = ret->value->type;
    if (found->isError()) {
      continue;
    }
    if (found->isUndefined()) {
      isUndefined = true;
      continue;
    }
    if (!type) {
      type = found;
      continue;
    }
    bool isAdapting = type != found && this->typeRelations->literalAdapts(ret->value, type);
    if (isAdapting) {
      continue;
    }
    ResultReturnsUnified common = this->commonReturn(fn, type, ret);
    if (common.isErr() && report) {
      this->state.getDiagnostics().error(ret->value->loc(), common.error().message);
    }
    if (common.isErr()) {
      return Type::error();
    }
    type = common.value();
  }
  if (!type) {
    ResultReturnsDefined defined = this->definedReturn(fn, returns, isUndefined);
    if (defined.isErr() && report) {
      this->state.getDiagnostics().error(fn->loc, defined.error().message);
    }
    return report ? Type::error() : nullptr;
  }
  if (isUndefined && !type->isVoid()) {
    type = Type::optional(type);
  }

  return type;
}

FunctionDecl *ReturnInferrer::freshCopy(const FunctionDecl *fn) {
  AstCloner cloner(this->state.getAst());
  FunctionDecl *copy = cloner.copyFunction(fn);
  copy->place = fn->place;
  copy->isAsync = fn->isAsync;
  const ClassMember *original = fn->member();
  if (original) {
    ClassMember copied{.cls = original->cls, .access = original->access};
    if (fn->isStatic()) {
      copied.method = StaticMethod{};
    } else if (fn->isConstructor()) {
      copied.method = Constructor{};
    }
    copy->role = std::move(copied);
  }
  copy->sym = fn->sym;
  copy->name = fn->name;
  copy->typeArgs = fn->typeArgs;
  copy->isInstance = fn->isInstance;
  copy->parent = fn->parent;
  copy->group = fn->group;
  copy->var = fn->var;
  copy->creationSeq = fn->creationSeq;
  if (!fn->listName.empty() && copy->params.size() > fn->listStart) {
    copy->params.resize(fn->listStart);
    copy->listName = fn->listName;
    copy->listStart = fn->listStart;
    copy->listType = fn->listType;
  }
  for (size_t i = 0; i < copy->params.size() && i < fn->params.size(); ++i) {
    copy->params[i].declaredType = fn->params[i].declaredType;
  }
  for (size_t i = copy->params.size(); i < fn->params.size(); ++i) {
    copy->params.push_back(fn->params[i]);
  }

  return copy;
}

}
