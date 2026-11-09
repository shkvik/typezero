#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/program/ast/ast_cloner/ast_cloner.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/stmt/return_checker/return_checker.h"
#include "compiler/semantic/declare/import_binder/import_binder.h"
#include "compiler/semantic/declare/module_declarer/module_declarer.h"
#include "compiler/semantic/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/late_classes/late_classes.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"

using llvm::cast;

namespace tz {

ReturnInferrer::ReturnInferrer(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.getScopes();
  this->classLinks = &all.getClassLinks();
  this->typeParams = &all.getTypeParams();
  this->exprChecker = &all.getExprChecker();
  this->lateClasses = &all.getLateClasses();
  this->asyncChecker = &all.getAsyncChecker();
  this->classChecker = &all.getClassChecker();
  this->importBinder = &all.getImportBinder();
  this->typeRelations = &all.getTypeRelations();
  this->functionChecker = &all.getFunctionChecker();
  this->generatorChecker = &all.getGeneratorChecker();
  this->genericClassChecker = &all.getGenericClassChecker();
}

bool ReturnInferrer::returnsValue(const Stmt *stmt) {
  if (!stmt) {
    return false;
  }
  auto caseReturns = [](const SwitchCase &c) {
    return llvm::any_of(c.body, ReturnInferrer::returnsValue);
  };
  switch (stmt->kind()) {
    case Stmt::Kind::Return: {
      const Expr *value = cast<ReturnStmt>(stmt)->value;
      auto *unary = llvm::dyn_cast_or_null<UnaryExpr>(value);
      return value && !(unary && unary->op == UnaryOp::Void);
    }
    case Stmt::Kind::Block:
      return llvm::any_of(cast<BlockStmt>(stmt)->body, ReturnInferrer::returnsValue);
    case Stmt::Kind::If: {
      const auto *branch = cast<IfStmt>(stmt);
      return ReturnInferrer::returnsValue(branch->thenStmt) ||
             ReturnInferrer::returnsValue(branch->elseStmt);
    }
    case Stmt::Kind::While:
      return ReturnInferrer::returnsValue(cast<WhileStmt>(stmt)->body);
    case Stmt::Kind::DoWhile:
      return ReturnInferrer::returnsValue(cast<DoWhileStmt>(stmt)->body);
    case Stmt::Kind::For:
      return ReturnInferrer::returnsValue(cast<ForStmt>(stmt)->body);
    case Stmt::Kind::ForOf:
      return ReturnInferrer::returnsValue(cast<ForOfStmt>(stmt)->body);
    case Stmt::Kind::Switch:
      return llvm::any_of(cast<SwitchStmt>(stmt)->cases, caseReturns);
    case Stmt::Kind::Labeled:
      return ReturnInferrer::returnsValue(cast<LabeledStmt>(stmt)->body);
    default:
      return false;
  }
}

ReturnInferrer::ResultNotCalledEarly ReturnInferrer::earlyCall(const FuncSymbol *fn) const {
  bool calledWhileLoading = fn->inferring && this->state.getReturnInference().isSuspended(fn);
  if (calledWhileLoading) {
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
  this->resolveParams(fn);
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
  saved.capture = this->state.getDiagnostics().capture();
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
    this->state.getDiagnostics().setCapture(nullptr);
    InstanceRecord *owner =
        fn && fn->cls() ? this->state.getLateClassStore().findLateOwner(fn->cls()) : nullptr;
    if (owner) {
      this->state.getGenericStore().rememberCheckingInstance(owner);
      this->state.getLateClassStore().captureLateErrorsOf(owner, this->state.getDiagnostics());
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
  this->state.getDiagnostics().setCapture(saved.capture);
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
  bool fromTemplate = decl && decl->generic && decl->cls() && decl->cls()->generic() &&
                      !this->lateClasses->hasClassExpr(decl->generic);
  if (fromTemplate) {
    fn->setReturnType(this->templateReturn(fn));
    return;
  }
  const Type *inherited = this->classLinks->inheritedReturnType(fn);
  if (inherited) {
    fn->setReturnType(inherited);
    return;
  }

  BodyState saved = this->enterBody(decl);
  bool trialClosure = decl->isClosure() && saved.functionStack.trial;
  bool muted = this->state.getDiagnostics().setMuted(trialClosure);
  bool topLevelEarly = !decl->isClosure() && !this->state.getModuleStates().isTopLevelDone();
  bool groupEarly = decl->group && !decl->group->finished;
  if (topLevelEarly || groupEarly) {
    this->state.getFunctionStack().rememberEarlyFunction(fn);
  }

  ReturnInference &inference = this->state.getReturnInference();
  if (inference.isInCycle(fn)) {
    this->inferCycle(inference.getCycleOf(fn));
  } else {
    this->checkAndInfer(decl);
  }

  this->state.getDiagnostics().setMuted(muted);
  this->leaveBody(saved);
}

void ReturnInferrer::checkAndInfer(FunctionDecl *decl) {
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
  this->functionChecker->checkFunctionBody(decl);
  this->state.getReturnInference().leaveInference();
  fn->inferring = false;
  functions.rememberReturnCollector(outerReturns);
  functions.rememberYieldCollector(outerYields);

  bool fallsOff = !ReturnChecker::alwaysReturns(decl->body);
  const Type *type = nullptr;
  if (decl->isGenerator) {
    type = this->generatorChecker->inferGeneratorType(fn, yields, returns, fallsOff, true);
  } else if (decl->isAsync) {
    type = this->asyncChecker->inferAsyncType(fn, returns, fallsOff, true);
  } else {
    type = this->unifyReturns(fn, returns, fallsOff, true);
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
  bool async = decl->isAsync;
  bool iterates =
      fn->getKnownReturnType()->isIterator() || fn->getKnownReturnType()->isAsyncIterator();
  const Type *returned = decl->isGenerator && iterates ? fn->getKnownReturnType()->returned()
                                                       : fn->getKnownReturnType();
  if (!returned->isError() && !returned->isVoid()) {
    for (ReturnStmt *ret : returns) {
      bool typed = ret->value && !ret->value->type->isError();
      bool settled = typed && async &&
                     AsyncChecker::awaitedType(ret->value->type) == ret->value->type &&
                     !ret->value->type->isUndefined();
      if (typed && (!async || settled)) {
        ret->value = this->typeRelations->convert(ret->value, returned);
      }
    }
  }
  if (iterates) {
    const Type *yielded = fn->getKnownReturnType()->yielded();
    for (YieldExpr *yield : yields) {
      bool typed = !yield->delegate && yield->value && !yield->value->type->isError();
      bool settled =
          typed && async && AsyncChecker::awaitedType(yield->value->type) == yield->value->type;
      if (typed && (!async || settled)) {
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

  bool fallsOff = !ReturnChecker::alwaysReturns(copy->body);
  const Type *found = nullptr;
  if (fn->decl->isGenerator) {
    found = this->generatorChecker->inferGeneratorType(fn, yields, returns, fallsOff, false);
  } else if (fn->decl->isAsync) {
    found = this->asyncChecker->inferAsyncType(fn, returns, fallsOff, false);
  } else {
    found = this->unifyReturns(fn, returns, fallsOff, false);
  }

  return found;
}

ReturnInferrer::ResultCycleTypeKnown ReturnInferrer::cycleType(const FuncSymbol *fn, int cycleSize,
                                                               bool failed) const {
  bool unknown = fn->getKnownReturnType()->isError();
  if (unknown && failed) {
    std::string message = ErrorTextCycleType::differ(fn->name);

    return err(ReturnInferrerCycleTypeError{
        .code = ReturnInferrerCycleTypeErrorCode::DIFFERENT_TYPES,
        .message = message,
    });
  }
  if (unknown && cycleSize == 1) {
    std::string message = ErrorTextCycleType::selfOnly(fn->name);

    return err(ReturnInferrerCycleTypeError{
        .code = ReturnInferrerCycleTypeErrorCode::ONLY_SELF_CALLS,
        .message = message,
    });
  }
  if (unknown) {
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
  bool muted = this->state.getDiagnostics().setMuted(true);
  bool failed = false;
  for (int round = 0; round < 8 && !failed; ++round) {
    bool changed = false;
    for (FuncSymbol *fn : cycle) {
      const Type *found = this->trialReturn(fn);
      if (!found) {
        continue;
      }
      const Type *next = fn->assumed ? LiteralTypes::unionUnify(fn->assumed, found) : found;
      if (!next || next->isError()) {
        failed = true;
        break;
      }
      if (next != fn->assumed) {
        fn->assumed = next;
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
  }
  this->state.getDiagnostics().setMuted(muted);
  for (size_t i = 0; i < cycle.size(); ++i) {
    this->state.getReturnInference().leaveInference();
  }

  for (FuncSymbol *fn : cycle) {
    fn->inferring = false;
    fn->setReturnType(fn->assumed && !failed ? fn->assumed : Type::error());
    fn->assumed = nullptr;
  }
  int cycleSize = int(cycle.size());
  for (FuncSymbol *fn : cycle) {
    ResultCycleTypeKnown known = this->cycleType(fn, cycleSize, failed);
    if (known.isErr()) {
      this->state.getDiagnostics().error(fn->loc, known.error().message);
    }
  }
}

std::vector<ReturnStmt *> ReturnInferrer::literalsLast(llvm::ArrayRef<ReturnStmt *> returns) {
  std::vector<ReturnStmt *> ordered;
  std::vector<ReturnStmt *> literals;
  for (ReturnStmt *ret : returns) {
    bool literal = ret->value && ret->value->type && ret->value->type->isInteger() &&
                   TypeRelations::isIntLiteralTree(ret->value);
    if (literal) {
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
  bool onlyUndefined = undefined && llvm::any_of(returns, withValue);
  if (onlyUndefined) {
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
  bool undefined = fallsOff;
  std::vector<ReturnStmt *> ordered = ReturnInferrer::literalsLast(returns);
  for (ReturnStmt *ret : ordered) {
    if (!ret->value) {
      undefined = true;
      continue;
    }
    const Type *found = ret->value->type;
    if (found->isError()) {
      continue;
    }
    if (found->isUndefined()) {
      undefined = true;
      continue;
    }
    if (!type) {
      type = found;
      continue;
    }
    bool adapts = type != found && this->typeRelations->literalAdapts(ret->value, type);
    if (adapts) {
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
    ResultReturnsDefined defined = this->definedReturn(fn, returns, undefined);
    if (defined.isErr() && report) {
      this->state.getDiagnostics().error(fn->loc, defined.error().message);
    }
    return report ? Type::error() : nullptr;
  }
  if (undefined && !type->isVoid()) {
    type = Type::optional(type);
  }

  return type;
}

const Type *ReturnInferrer::instanceParam(FuncSymbol *fn, const Type *type) {
  if (!type) {
    return nullptr;
  }
  FunctionDecl *decl = fn->decl;
  FuncSymbol *generic = decl->generic->sym;
  const Type *instance = this->genericClassChecker->instanceValueType(decl->cls(), type);

  return this->typeParams->substituteValue(instance, generic->typeParams, fn->typeParams);
}

ReturnInferrer::ResultParamTypeKnown ReturnInferrer::defaultType(const Param &param,
                                                                 const Type *found) const {
  const Type *type = found ? found : Type::error();
  if (type->isVoid() || type->isUndefined()) {
    std::string message = ErrorTextDefaultType::unknown(param.label());

    return err(ReturnInferrerDefaultTypeError{
        .code = ReturnInferrerDefaultTypeErrorCode::UNKNOWN_FROM_DEFAULT,
        .message = message,
    });
  }

  return ok(type);
}

void ReturnInferrer::resolveParams(FuncSymbol *fn) {
  if (!fn->pendingParams) {
    return;
  }
  fn->pendingParams = false;
  FunctionDecl *decl = fn->decl;
  FunctionDecl *generic = decl->generic;
  if (generic && decl->cls() && decl->cls()->generic()) {
    this->resolveParams(generic->sym);
    for (size_t i = 0; i < fn->params.size() && i < generic->sym->params.size(); ++i) {
      fn->params[i] = this->instanceParam(fn, generic->sym->params[i]);
    }
    for (size_t i = 0; i < decl->params.size() && i < generic->params.size(); ++i) {
      decl->params[i].declaredType = this->instanceParam(fn, generic->params[i].declaredType);
    }
    return;
  }

  std::vector<const Type *> found(decl->params.size(), nullptr);
  auto failed = [](const Type *type) { return type && type->isError(); };
  for (bool report : {false, true}) {
    if (report && llvm::none_of(found, failed)) {
      break;
    }
    BodyState saved = this->enterBody(decl);
    bool muted = this->state.getDiagnostics().setMuted(!report);
    FunctionDecl *copy = this->freshCopy(decl);
    this->state.getFunctionStack().enterFunction(copy);
    if (copy->takesThis()) {
      ClassChecker::ResultNewThisSymbol declared = this->classChecker->declareThis(copy);
      if (declared.isErr()) {
        this->state.getDiagnostics().error(copy->loc(), declared.error().message);
      }
    }
    ClassContext &classContext = this->state.getClassContext();
    classContext.enterClass(copy->isClosure() ? classContext.getCurrentClass() : copy->cls());
    this->state.getScopeStack().openScope();
    for (size_t i = 0; i < decl->params.size() && i < copy->params.size(); ++i) {
      Param &param = copy->params[i];
      const Type *type = decl->params[i].declaredType;
      if (!type) {
        Expr *value = this->exprChecker->checkExpr(param.defaultValue());
        found[i] = value->type;
        type = value->type;
      }
      this->scopes->declareVar(param.name, param.loc, type, false);
    }
    this->scopes->closeScope();
    this->state.getFunctionStack().leaveFunction();
    classContext.leaveClass();
    this->state.getDiagnostics().setMuted(muted);
    this->leaveBody(saved);
  }

  for (size_t i = 0; i < decl->params.size(); ++i) {
    Param &param = decl->params[i];
    if (param.declaredType) {
      continue;
    }
    ResultParamTypeKnown known = this->defaultType(param, found[i]);
    if (known.isErr()) {
      this->state.getDiagnostics().error(param.loc, known.error().message);
    }
    const Type *type = known.isOk() ? known.value() : Type::error();
    param.declaredType = type;
    fn->params[i] = Type::optional(type);
  }
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
