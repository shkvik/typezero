#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/bodies/functions/nested_functions/nested_functions.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/bodies/stmt/stmt_checker/stmt_checker.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ClosureChecker::ClosureChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->typeParams = &all.typeParams;
  this->stmtChecker = &all.stmtChecker;
  this->asyncChecker = &all.asyncChecker;
  this->classChecker = &all.classChecker;
  this->typeDeclarer = &all.typeDeclarer;
  this->typeResolver = &all.typeResolver;
  this->argListChecker = &all.argListChecker;
  this->returnInferrer = &all.returnInferrer;
  this->functionChecker = &all.functionChecker;
  this->nestedFunctions = &all.nestedFunctions;
  this->generatorChecker = &all.generatorChecker;
  this->predicateChecker = &all.predicateChecker;
}

Signature ClosureChecker::signatureOf(const Type *functionType) const {
  Signature signature;
  signature.params.assign(functionType->params().begin(), functionType->params().end());
  signature.minArgs = functionType->minArgs();
  signature.ret = functionType->ret();
  signature.voidResult = functionType->ret()->isVoid();
  signature.hasThis = functionType->hasThis();

  return signature;
}

ClosureChecker::ResultExprNotGeneric
ClosureChecker::notGeneric(const FunctionDecl *functionDecl) const {
  if (!functionDecl->typeParams.empty()) {
    auto message = ErrorTextNotGeneric::generic();

    return err(ClosureCheckerNotGenericError{
        .code = ClosureCheckerNotGenericErrorCode::GENERIC_FUNCTION_EXPRESSION,
        .message = message,
    });
  }

  return ok(functionDecl);
}

ClosureChecker::ResultParamTypeAgree
ClosureChecker::paramAgrees(const Param &param, const Type *passed, const Type *given) const {
  if (given != nullptr && passed != given && !passed->isError() && !given->isError() &&
      !param.rest()) {
    auto message = ErrorTextParamAgrees::declared(param.label(), passed->name(), given->name());

    return err(ClosureCheckerParamAgreesError{
        .code = ClosureCheckerParamAgreesErrorCode::DECLARED_DIFFERENTLY,
        .message = message,
    });
  }

  return ok(passed);
}

ClosureChecker::ResultDefaultMatters ClosureChecker::defaultMatters(const Param &param,
                                                                    const Type *given) const {
  if (param.defaultValue() != nullptr && !given->isOptional()) {
    auto message = ErrorTextDefaultMatters::unused(param.label());

    return err(ClosureCheckerDefaultMattersError{
        .code = ClosureCheckerDefaultMattersErrorCode::DEFAULT_NEVER_USED,
        .message = message,
    });
  }

  return ok(&param);
}

ClosureChecker::ResultParamTypeKnown ClosureChecker::paramTypeKnown(const Param &param) const {
  if (!param.type) {
    auto message = ErrorTextParamTypeKnown::needsType(param.label());

    return err(ClosureCheckerParamTypeKnownError{
        .code = ClosureCheckerParamTypeKnownErrorCode::NEEDS_TYPE,
        .message = message,
    });
  }

  return ok(&param);
}

ClosureChecker::ResultThisParamValid ClosureChecker::thisParam(const FunctionDecl *functionDecl,
                                                               const Param &param) const {
  if (functionDecl->isArrow()) {
    auto message = ErrorTextThisParam::arrowThis();

    return err(ClosureCheckerThisParamError{
        .code = ClosureCheckerThisParamErrorCode::ARROW_THIS_PARAMETER,
        .message = message,
    });
  }

  return ok(&param);
}

ClosureChecker::ResultRestParamArray ClosureChecker::restParam(const Param &param,
                                                               const Type *type) const {
  if (!type->isArray() && !type->isListParam() && !type->isTuple() && !type->isError()) {
    auto message = ErrorTextRestParam::notArray(param.name);

    return err(ClosureCheckerRestParamError{
        .code = ClosureCheckerRestParamErrorCode::REST_NOT_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

ClosureChecker::ResultParamOrderGood ClosureChecker::paramOrder(const Param &param,
                                                                bool afterOptional) const {
  if (afterOptional) {
    auto message = ErrorTextParamOrder::order();

    return err(ClosureCheckerParamOrderError{
        .code = ClosureCheckerParamOrderErrorCode::REQUIRED_AFTER_OPTIONAL,
        .message = message,
    });
  }

  return ok(&param);
}

ClosureChecker::ResultPromiseAwaited
ClosureChecker::promiseAwaited(const Signature *signature) const {
  if (!signature->asyncVoid) {
    auto message = ErrorTextPromiseAwaited::dropped();

    return err(ClosureCheckerPromiseAwaitedError{
        .code = ClosureCheckerPromiseAwaitedErrorCode::PROMISE_DROPPED,
        .message = message,
    });
  }

  return ok(signature);
}

Expr *ClosureChecker::checkFunctionExpr(FunctionExpr *functionExpr, const Signature *signature) {
  FunctionDecl *functionDecl = functionExpr->fn;
  auto *function = this->makeClosureSymbol(functionDecl);
  bool isGeneric = this->reportsGenericClosure(functionExpr);
  ClosureParams params{
      .sym = function,
      .thisFromContext = this->addsContextThis(functionExpr, signature),
      .afterOptional = false,
  };
  size_t named = functionDecl->params.size();
  for (size_t index = 0; index < named; ++index) {
    this->walkClosureParam(functionDecl, index, signature, params);
  }
  this->padHiddenParams(functionExpr, function, signature);
  this->argListChecker->expandArgumentList(functionDecl, function);
  this->walkClosureReturn(functionExpr, function, signature);
  if (isGeneric) {
    this->state.getScopeStack().closeTypeScope();
  }
  this->inferRecursiveReturn(functionDecl, function);
  this->walkInitializerBody(functionDecl);
  functionExpr->type = this->returnInferrer->functionType(function);

  return functionExpr;
}

FuncSymbol *ClosureChecker::makeClosureSymbol(FunctionDecl *functionDecl) {
  auto *function = this->state.getAst().newFunc();
  function->setCompleter(this->state.getCompleter());
  function->name = functionDecl->name;
  function->loc = functionDecl->loc();
  function->decl = functionDecl;
  functionDecl->sym = function;
  functionDecl->parent = this->state.getFunctionStack().getCurrentDecl();
  functionDecl->creationSeq = this->state.getReturnInference().takeSequenceNumber();

  return function;
}

bool ClosureChecker::reportsGenericClosure(FunctionExpr *functionExpr) {
  FunctionDecl *functionDecl = functionExpr->fn;
  auto plain = this->notGeneric(functionDecl);
  if (plain.isOk()) {
    return false;
  }
  this->state.getDiagnostics().error(functionExpr->loc(), plain.error().message);
  functionDecl->typeArgs.assign(functionDecl->typeParams.size(), Type::error());
  this->state.getScopeStack().openTypeScope();
  this->typeParams->bindTypeParams(functionDecl->typeParams, functionDecl->typeArgs);

  return true;
}

bool ClosureChecker::addsContextThis(FunctionExpr *functionExpr, const Signature *signature) {
  FunctionDecl *functionDecl = functionExpr->fn;
  bool isWanted = signature != nullptr && signature->hasThis && !signature->params.empty() &&
                  (functionDecl->params.empty() || functionDecl->params[0].name != "this" ||
                   functionDecl->isArrow());
  if (!isWanted) {
    return false;
  }
  Param self;
  self.name = functionDecl->isArrow() ? this->state.getAst().hiddenName() : llvm::StringRef("this");
  self.loc = functionExpr->loc();
  functionDecl->params.insert(functionDecl->params.begin(), self);

  return true;
}

void ClosureChecker::walkClosureParam(FunctionDecl *functionDecl, size_t index,
                                      const Signature *signature, ClosureParams &params) {
  FuncSymbol *function = params.sym;
  Param &param = functionDecl->params[index];
  const Type *given =
      signature != nullptr && index < signature->params.size() ? signature->params[index] : nullptr;
  bool isMayOmit = param.optional() || param.defaultValue() != nullptr;
  auto types = this->closureParamTypes(param, given);
  if (index == 0) {
    this->walkClosureThis(functionDecl, param, isMayOmit, params);
  }
  if (param.rest()) {
    auto array = this->restParam(param, types.type);
    if (array.isErr()) {
      this->state.getDiagnostics().error(param.loc, array.error().message);
    }
    function->hasRest = true;
    types.passed = types.type;
  } else if (!isMayOmit) {
    auto order = this->paramOrder(param, params.afterOptional);
    if (order.isErr()) {
      this->state.getDiagnostics().error(param.loc, order.error().message);
    }
    function->minArgs = index + 1;
  }
  params.afterOptional |= isMayOmit;
  param.declaredType = types.type;
  function->params.push_back(types.passed);
}

ClosureChecker::ParamTypes ClosureChecker::closureParamTypes(const Param &param,
                                                             const Type *given) {
  if (param.type.has_value()) {
    return this->writtenParamTypes(param, given);
  }
  if (given != nullptr) {
    auto matters = this->defaultMatters(param, given);
    if (matters.isErr()) {
      this->state.getDiagnostics().error(param.defaultValue()->loc(), matters.error().message);
    }
    return ParamTypes{
        .type = param.defaultValue() != nullptr ? given->nonOptional() : given,
        .passed = given,
    };
  }
  const Type *literal =
      param.defaultValue() != nullptr ? SharedHelpers::literalType(param.defaultValue()) : nullptr;
  if (literal != nullptr) {
    return ParamTypes{
        .type = literal,
        .passed = Type::optional(literal),
    };
  }
  auto known = this->paramTypeKnown(param);
  if (known.isErr()) {
    this->state.getDiagnostics().error(param.loc, known.error().message);
  }

  return ParamTypes{
      .type = Type::error(),
      .passed = Type::error(),
  };
}

ClosureChecker::ParamTypes ClosureChecker::writtenParamTypes(const Param &param,
                                                             const Type *given) {
  bool isMayOmit = param.optional() || param.defaultValue() != nullptr;
  this->state.getTypeContext().enterListContext(param.rest());
  const auto *type = this->typeResolver->resolveType(*param.type);
  this->state.getTypeContext().leaveListContext();
  if (param.optional()) {
    type = Type::optional(type);
  }
  const Type *passed = isMayOmit ? Type::optional(type) : type;
  auto agrees = this->paramAgrees(param, passed, given);
  if (agrees.isErr()) {
    this->state.getDiagnostics().error(param.loc, agrees.error().message);
    return ParamTypes{
        .type = param.defaultValue() != nullptr ? given->nonOptional() : given,
        .passed = given,
    };
  }

  return ParamTypes{
      .type = type,
      .passed = passed,
  };
}

void ClosureChecker::walkClosureThis(const FunctionDecl *functionDecl, const Param &param,
                                     bool mayOmit, ClosureParams &params) {
  if (params.thisFromContext) {
    params.sym->hasThis = true;
    return;
  }
  if (param.name != "this" || !param.type.has_value() || param.rest()) {
    return;
  }
  auto valid = this->thisParam(functionDecl, param);
  if (valid.isErr()) {
    this->state.getDiagnostics().error(param.loc, valid.error().message);
    return;
  }
  params.sym->hasThis = !mayOmit;
}

void ClosureChecker::padHiddenParams(FunctionExpr *functionExpr, FuncSymbol *function,
                                     const Signature *signature) {
  FunctionDecl *functionDecl = functionExpr->fn;
  if (signature == nullptr || !signature->pad || function->hasRest) {
    return;
  }
  size_t named = functionDecl->params.size();
  for (size_t index = named; index < signature->params.size(); ++index) {
    Param hidden;
    hidden.name = this->state.getAst().hiddenName();
    hidden.loc = functionExpr->loc();
    hidden.declaredType = signature->params[index];
    functionDecl->params.push_back(hidden);
    function->params.push_back(signature->params[index]);
  }
  function->minArgs = signature->minArgs;
}

void ClosureChecker::walkClosureReturn(FunctionExpr *functionExpr, FuncSymbol *function,
                                       const Signature *signature) {
  FunctionDecl *functionDecl = functionExpr->fn;
  if (functionDecl->isGenerator) {
    return this->walkGeneratorReturn(functionDecl, function, signature);
  }
  if (functionDecl->isAsync) {
    return this->walkAsyncReturn(functionExpr, function, signature);
  }

  return this->walkPlainReturn(functionDecl, function, signature);
}

void ClosureChecker::walkGeneratorReturn(FunctionDecl *functionDecl, FuncSymbol *function,
                                         const Signature *signature) {
  if (functionDecl->returnType.has_value()) {
    const auto *declared = this->typeResolver->resolveTypeOrVoid(*functionDecl->returnType);
    auto generator = this->generatorChecker->generatorReturnType(functionDecl, declared);
    if (generator.isErr()) {
      this->state.getDiagnostics().error(functionDecl->returnType->loc, generator.error().message);
    }
    function->setReturnType(generator.isOk() ? generator.value() : Type::error());
    return;
  }
  bool isFitting =
      signature != nullptr && signature->ret != nullptr &&
      (functionDecl->isAsync ? signature->ret->isAsyncIterator() : signature->ret->isIterator());
  if (isFitting) {
    function->setReturnType(signature->ret);
  }
}

void ClosureChecker::walkAsyncReturn(FunctionExpr *functionExpr, FuncSymbol *function,
                                     const Signature *signature) {
  FunctionDecl *functionDecl = functionExpr->fn;
  if (signature != nullptr && signature->voidResult) {
    auto awaited = this->promiseAwaited(signature);
    if (awaited.isErr()) {
      this->state.getDiagnostics().error(functionExpr->loc(), awaited.error().message);
    }
    function->setReturnType(Type::voidTy());
    function->discardsResult = true;
    return;
  }
  if (functionDecl->returnType.has_value()) {
    const auto *declared = this->typeResolver->resolveTypeOrVoid(*functionDecl->returnType);
    const auto *returned = this->asyncChecker->asyncReturnType(functionDecl, declared);
    function->setReturnType(returned);
    return;
  }
  if (signature != nullptr && signature->ret != nullptr && signature->ret->isPromise()) {
    function->setReturnType(signature->ret);
    return;
  }
  if (!this->returnInferrer->isReturningValue(functionDecl->body)) {
    function->setReturnType(Type::promise(Type::voidTy()));
  }
}

void ClosureChecker::walkPlainReturn(FunctionDecl *functionDecl, FuncSymbol *function,
                                     const Signature *signature) {
  if (signature != nullptr && signature->voidResult) {
    function->setReturnType(Type::voidTy());
    function->discardsResult = true;
    return;
  }
  if (functionDecl->returnType.has_value()) {
    function->setReturnType(this->predicateChecker->resolveReturnType(functionDecl, function));
    return;
  }
  if (signature != nullptr && signature->ret != nullptr) {
    function->setReturnType(signature->ret);
    return;
  }
  if (!this->returnInferrer->isReturningValue(functionDecl->body)) {
    function->setReturnType(Type::voidTy());
  }
}

void ClosureChecker::inferRecursiveReturn(FunctionDecl *functionDecl, FuncSymbol *function) {
  if (function->hasReturnType() || !this->isRecursiveClosure(functionDecl)) {
    return;
  }
  this->state.getReturnInference().addCycle({function});
  auto saved = this->returnInferrer->enterBody(functionDecl);
  bool isOwnScope = !functionDecl->ownName().empty();
  if (isOwnScope) {
    this->declareOwnName(functionDecl);
  }
  this->returnInferrer->inferCycle(this->state.getReturnInference().getCycleOf(function));
  if (isOwnScope) {
    this->scopes->closeScope();
  }
  this->returnInferrer->leaveBody(saved);
}

bool ClosureChecker::isRecursiveClosure(FunctionDecl *functionDecl) {
  auto names = llvm::StringSet<>();
  AstWalker::collectNames(functionDecl->body, names);
  bool isRecursive = !functionDecl->ownName().empty() && names.contains(functionDecl->ownName());
  for (const auto &entry : names) {
    VarSymbol *variable = this->scopes->lookup(entry.getKey()).var();
    isRecursive |= variable != nullptr && variable->lambda() == functionDecl;
  }

  return isRecursive;
}

void ClosureChecker::walkInitializerBody(FunctionDecl *functionDecl) {
  auto &classContext = this->state.getClassContext();
  auto *initializer = classContext.getInitializer();
  bool isInInitializer =
      initializer != nullptr && this->state.getFunctionStack().getCurrentDecl() == initializer;
  if (!isInInitializer) {
    return this->walkClosureBody(functionDecl);
  }
  bool isThisReady = this->classChecker->thisReady(this->state.getFlowState().getFlow()).isOk();
  classContext.enterInitializerClosure(isThisReady);
  this->walkClosureBody(functionDecl);
  classContext.leaveInitializerClosure();
}

void ClosureChecker::declareOwnName(FunctionDecl *functionDecl) {
  this->state.getScopeStack().openScope();
  auto *self = this->state.getAst().newVar();
  self->name = functionDecl->ownName();
  self->loc = functionDecl->loc();
  self->isConst = true;
  self->holds = Lambda{.lambda = functionDecl};
  unsigned declSeq = this->state.getReturnInference().takeSequenceNumber();
  self->place = FunctionLocalVar{.owner = functionDecl, .state = {.declSeq = declSeq}};
  auto added = this->scopes->declare(functionDecl->ownName(), {VarEntry{.var = self}});
  if (added.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), added.error().message);
  }
  if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
    auto previousEntry = this->scopes->lookup(functionDecl->ownName());
    auto declaredHere = ErrorTextDeclare::previous();
    this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
  }
}

void ClosureChecker::walkClosureBody(FunctionDecl *functionDecl) {
  auto saved = this->returnInferrer->enterBody(functionDecl);
  auto fixed = [](Term term) {
    return (term.var == nullptr || (term.var->isConst && !term.length)) &&
           (term.var2 == nullptr || (term.var2->isConst && !term.length2));
  };
  for (const Facts::Fact &fact : saved.flow.facts.all()) {
    if (fixed(fact.lhs) && fixed(fact.rhs)) {
      this->state.getFlowState().getFacts().add(fact.lhs, fact.rhs, fact.offset);
    }
  }
  for (const VarSymbol *variable : saved.flow.defined) {
    if (variable->isConst) {
      this->state.getFlowState().markDefined(variable);
    }
  }
  for (const auto &[var, type] : saved.flow.narrowed) {
    if (var->isConst) {
      this->state.getFlowState().addNarrowed(var, type);
    }
  }
  for (const VarSymbol *variable : saved.flow.okResults) {
    if (variable->isConst) {
      this->state.getFlowState().markOkResult(variable);
    }
  }
  for (const VarSymbol *variable : saved.flow.errResults) {
    if (variable->isConst) {
      this->state.getFlowState().markErrResult(variable);
    }
  }
  bool isOwnScope = functionDecl->isExpression() && !functionDecl->ownName().empty();
  if (isOwnScope) {
    this->declareOwnName(functionDecl);
  }
  if (functionDecl->sym->hasReturnType()) {
    this->functionChecker->walkFunctionBody(functionDecl);
  } else {
    this->returnInferrer->inferFromBody(functionDecl);
  }
  if (isOwnScope) {
    this->scopes->closeScope();
  }
  this->returnInferrer->leaveBody(saved);
}

void ClosureChecker::walkStatements(BlockStmt *block) {
  this->typeDeclarer->declareTypes(block->body);
  auto *group = this->nestedFunctions->declareNested(block);
  if (group != nullptr) {
    this->state.getFunctionStack().enterGroup(group);
  }
  BlockNames names{this->state.getFunctionStack().getFunctionDepth(), {}};
  for (Stmt *stmt : block->body) {
    auto *varDecl = dyn_cast<VarDeclStmt>(stmt);
    if (varDecl != nullptr) {
      for (const VarDeclarator &declarator : varDecl->decls) {
        names.names.push_back(declarator.name);
      }
    }
  }
  this->state.getScopeStack().enterBlockNames(std::move(names));
  for (size_t index = 0; index < block->body.size(); ++index) {
    if (group != nullptr && index == group->firstUse) {
      group->creationSeq = this->state.getReturnInference().takeSequenceNumber();
      for (FunctionDecl *functionDecl : group->members) {
        LocalVarState *localState = functionDecl->var->local();
        if (localState != nullptr) {
          localState->declSeq = group->creationSeq;
        }
      }
    }
    this->state.getBlockPosition().rememberBlockPosition(block->body, index);
    this->stmtChecker->walkStmt(block->body[index]);
    auto *decided = dyn_cast<IfStmt>(block->body[index]);
    if (this->state.getGenericStore().isCheckingInstance() && decided != nullptr &&
        isa<BoolLitExpr>(decided->cond) && this->state.getFlowState().isUnreachable() &&
        (group == nullptr || group->firstUse <= index) &&
        llvm::none_of(llvm::ArrayRef<Stmt *>(block->body).drop_front(index + 1),
                      [](const Stmt *s) { return isa<FunctionDecl>(s) || isa<ClassDecl>(s); })) {
      block->body.resize(index + 1);
      break;
    }
  }
  this->state.getScopeStack().leaveBlockNames();
  if (group != nullptr) {
    this->nestedFunctions->finishGroup(group);
    this->state.getFunctionStack().leaveGroup();
  }
}

}
