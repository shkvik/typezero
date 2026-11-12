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

namespace {

constexpr const char *kThisName = "this";

}

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
  llvm::ArrayRef<const Type *> params = functionType->params();

  return Signature{
      .params = std::vector<const Type *>(params.begin(), params.end()),
      .minArgs = functionType->minArgs(),
      .ret = functionType->ret(),
      .voidResult = functionType->ret()->isVoid(),
      .hasThis = functionType->hasThis(),
  };
}

ClosureChecker::ResultExprNotGeneric
ClosureChecker::checkNotGeneric(const FunctionDecl *functionDecl) const {
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
ClosureChecker::checkParamAgrees(const Param &param, const Type *passed, const Type *given) const {
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

ClosureChecker::ResultDefaultMatters ClosureChecker::checkDefaultMatters(const Param &param,
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

ClosureChecker::ResultParamTypeKnown ClosureChecker::checkParamTypeKnown(const Param &param) const {
  if (!param.type) {
    auto message = ErrorTextParamTypeKnown::needsType(param.label());

    return err(ClosureCheckerParamTypeKnownError{
        .code = ClosureCheckerParamTypeKnownErrorCode::NEEDS_TYPE,
        .message = message,
    });
  }

  return ok(&param);
}

ClosureChecker::ResultThisParamValid
ClosureChecker::checkThisParam(const FunctionDecl *functionDecl, const Param &param) const {
  if (functionDecl->isArrow()) {
    auto message = ErrorTextThisParam::arrowThis();

    return err(ClosureCheckerThisParamError{
        .code = ClosureCheckerThisParamErrorCode::ARROW_THIS_PARAMETER,
        .message = message,
    });
  }

  return ok(&param);
}

ClosureChecker::ResultRestParamArray ClosureChecker::checkRestParam(const Param &param,
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

ClosureChecker::ResultParamOrderGood ClosureChecker::checkParamOrder(const Param &param,
                                                                     bool isAfterOptional) const {
  if (isAfterOptional) {
    auto message = ErrorTextParamOrder::order();

    return err(ClosureCheckerParamOrderError{
        .code = ClosureCheckerParamOrderErrorCode::REQUIRED_AFTER_OPTIONAL,
        .message = message,
    });
  }

  return ok(&param);
}

ClosureChecker::ResultPromiseAwaited
ClosureChecker::checkPromiseAwaited(const Signature *signature) const {
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
  bool isGeneric = this->reportGenericClosure(functionExpr);
  auto params = ClosureParams{
      .function = function,
      .isThisFromContext = this->addContextThis(functionExpr, signature),
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

bool ClosureChecker::reportGenericClosure(FunctionExpr *functionExpr) {
  FunctionDecl *functionDecl = functionExpr->fn;
  auto plain = this->checkNotGeneric(functionDecl);
  if (plain.isOk()) {
    return false;
  }
  this->state.getDiagnostics().error(functionExpr->loc(), plain.error().message);
  functionDecl->typeArgs.assign(functionDecl->typeParams.size(), Type::error());
  this->state.getScopeStack().openTypeScope();
  this->typeParams->bindTypeParams(functionDecl->typeParams, functionDecl->typeArgs);

  return true;
}

bool ClosureChecker::addContextThis(FunctionExpr *functionExpr, const Signature *signature) {
  FunctionDecl *functionDecl = functionExpr->fn;
  bool isThisWritten = !functionDecl->params.empty() && functionDecl->params[0].name == kThisName &&
                       !functionDecl->isArrow();
  bool isWanted =
      signature != nullptr && signature->hasThis && !signature->params.empty() && !isThisWritten;
  if (!isWanted) {
    return false;
  }
  llvm::StringRef name =
      functionDecl->isArrow() ? this->state.getAst().hiddenName() : llvm::StringRef(kThisName);
  auto self = Param{
      .name = name,
      .loc = functionExpr->loc(),
  };
  functionDecl->params.insert(functionDecl->params.begin(), self);

  return true;
}

void ClosureChecker::walkClosureParam(FunctionDecl *functionDecl, size_t index,
                                      const Signature *signature, const ClosureParams &params) {
  FuncSymbol *function = params.function;
  Param &param = functionDecl->params[index];
  const Type *given =
      signature != nullptr && index < signature->params.size() ? signature->params[index] : nullptr;
  bool isOmittable = this->isOmittableParam(param);
  auto types = this->closureParamTypes(param, given);
  if (index == 0) {
    this->walkClosureThis(functionDecl, param, isOmittable, params);
  }
  param.declaredType = types.type;
  if (param.rest()) {
    return this->walkClosureRest(function, param, types.type);
  }
  if (!isOmittable) {
    this->walkRequiredParam(functionDecl, function, index);
  }
  function->params.push_back(types.passed);
}

bool ClosureChecker::isOmittableParam(const Param &param) const {
  return param.optional() || param.defaultValue() != nullptr;
}

void ClosureChecker::walkClosureRest(FuncSymbol *function, const Param &param, const Type *type) {
  auto array = this->checkRestParam(param, type);
  if (array.isErr()) {
    this->state.getDiagnostics().error(param.loc, array.error().message);
  }
  function->hasRest = true;
  function->params.push_back(type);
}

void ClosureChecker::walkRequiredParam(const FunctionDecl *functionDecl, FuncSymbol *function,
                                       size_t index) {
  const Param &param = functionDecl->params[index];
  bool isAfterOptional = this->hasOmittableBefore(functionDecl, index);
  auto order = this->checkParamOrder(param, isAfterOptional);
  if (order.isErr()) {
    this->state.getDiagnostics().error(param.loc, order.error().message);
  }
  function->minArgs = index + 1;
}

bool ClosureChecker::hasOmittableBefore(const FunctionDecl *functionDecl, size_t index) const {
  for (size_t earlier = 0; earlier < index; ++earlier) {
    if (this->isOmittableParam(functionDecl->params[earlier])) {
      return true;
    }
  }

  return false;
}

ClosureChecker::ParamTypes ClosureChecker::closureParamTypes(const Param &param,
                                                             const Type *given) {
  if (param.type.has_value()) {
    return this->writtenParamTypes(param, given);
  }
  if (given != nullptr) {
    return this->givenParamTypes(param, given);
  }
  const Type *literal =
      param.defaultValue() != nullptr ? SharedHelpers::literalType(param.defaultValue()) : nullptr;
  if (literal != nullptr) {
    return ParamTypes{
        .type = literal,
        .passed = Type::optional(literal),
    };
  }
  auto known = this->checkParamTypeKnown(param);
  if (known.isErr()) {
    this->state.getDiagnostics().error(param.loc, known.error().message);
  }

  return ParamTypes{
      .type = Type::error(),
      .passed = Type::error(),
  };
}

ClosureChecker::ParamTypes ClosureChecker::givenParamTypes(const Param &param, const Type *given) {
  auto matters = this->checkDefaultMatters(param, given);
  if (matters.isErr()) {
    this->state.getDiagnostics().error(param.defaultValue()->loc(), matters.error().message);
  }

  return ParamTypes{
      .type = this->givenInnerType(param, given),
      .passed = given,
  };
}

const Type *ClosureChecker::givenInnerType(const Param &param, const Type *given) const {
  if (param.defaultValue() != nullptr) {
    return given->nonOptional();
  }

  return given;
}

ClosureChecker::ParamTypes ClosureChecker::writtenParamTypes(const Param &param,
                                                             const Type *given) {
  this->state.getTypeContext().enterListContext(param.rest());
  const auto *written = this->typeResolver->resolveType(*param.type);
  this->state.getTypeContext().leaveListContext();
  const Type *type = param.optional() ? Type::optional(written) : written;
  const Type *passed = this->isOmittableParam(param) ? Type::optional(type) : type;
  auto agrees = this->checkParamAgrees(param, passed, given);
  if (agrees.isErr()) {
    this->state.getDiagnostics().error(param.loc, agrees.error().message);
    return ParamTypes{
        .type = this->givenInnerType(param, given),
        .passed = given,
    };
  }

  return ParamTypes{
      .type = type,
      .passed = passed,
  };
}

void ClosureChecker::walkClosureThis(const FunctionDecl *functionDecl, const Param &param,
                                     bool isParamOmittable, const ClosureParams &params) {
  if (params.isThisFromContext) {
    params.function->hasThis = true;
    return;
  }
  if (param.name != kThisName || !param.type.has_value() || param.rest()) {
    return;
  }
  auto valid = this->checkThisParam(functionDecl, param);
  if (valid.isErr()) {
    this->state.getDiagnostics().error(param.loc, valid.error().message);
    return;
  }
  params.function->hasThis = !isParamOmittable;
}

void ClosureChecker::padHiddenParams(FunctionExpr *functionExpr, FuncSymbol *function,
                                     const Signature *signature) {
  FunctionDecl *functionDecl = functionExpr->fn;
  if (signature == nullptr || !signature->pad || function->hasRest) {
    return;
  }
  size_t named = functionDecl->params.size();
  for (size_t index = named; index < signature->params.size(); ++index) {
    auto hidden = Param{
        .name = this->state.getAst().hiddenName(),
        .loc = functionExpr->loc(),
        .declaredType = signature->params[index],
    };
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
    return this->walkWrittenGeneratorReturn(functionDecl, function);
  }
  if (signature == nullptr || signature->ret == nullptr) {
    return;
  }
  bool isIteratorResult =
      functionDecl->isAsync ? signature->ret->isAsyncIterator() : signature->ret->isIterator();
  if (isIteratorResult) {
    function->setReturnType(signature->ret);
  }
}

void ClosureChecker::walkWrittenGeneratorReturn(FunctionDecl *functionDecl, FuncSymbol *function) {
  const auto *declared = this->typeResolver->resolveTypeOrVoid(*functionDecl->returnType);
  auto generator = this->generatorChecker->checkGeneratorReturnType(functionDecl, declared);
  if (generator.isErr()) {
    this->state.getDiagnostics().error(functionDecl->returnType->loc, generator.error().message);
  }
  const Type *returned = generator.isOk() ? generator.value() : Type::error();
  function->setReturnType(returned);
}

void ClosureChecker::walkAsyncReturn(FunctionExpr *functionExpr, FuncSymbol *function,
                                     const Signature *signature) {
  FunctionDecl *functionDecl = functionExpr->fn;
  if (signature != nullptr && signature->voidResult) {
    auto awaited = this->checkPromiseAwaited(signature);
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
    const auto *declared = this->predicateChecker->resolveReturnType(functionDecl, function);
    function->setReturnType(declared);
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
  auto cycle = this->state.getReturnInference().getCycleOf(function);
  this->returnInferrer->inferCycle(cycle);
  if (isOwnScope) {
    this->scopes->closeScope();
  }
  this->returnInferrer->leaveBody(saved);
}

bool ClosureChecker::isRecursiveClosure(FunctionDecl *functionDecl) {
  auto names = llvm::StringSet<>();
  AstWalker::collectNames(functionDecl->body, names);
  if (!functionDecl->ownName().empty() && names.contains(functionDecl->ownName())) {
    return true;
  }
  for (const auto &entry : names) {
    auto found = this->scopes->lookup(entry.getKey());
    VarSymbol *variable = found.var();
    if (variable != nullptr && variable->lambda() == functionDecl) {
      return true;
    }
  }

  return false;
}

void ClosureChecker::walkInitializerBody(FunctionDecl *functionDecl) {
  auto &classContext = this->state.getClassContext();
  auto *initializer = classContext.getInitializer();
  bool isInInitializer =
      initializer != nullptr && this->state.getFunctionStack().getCurrentDecl() == initializer;
  if (!isInInitializer) {
    return this->walkClosureBody(functionDecl);
  }
  const Flow &flow = this->state.getFlowState().getFlow();
  auto ready = this->classChecker->checkThisReady(flow);
  classContext.enterInitializerClosure(ready.isOk());
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
  if (added.isOk()) {
    return;
  }
  this->state.getDiagnostics().error(functionDecl->loc(), added.error().message);
  if (added.error().code == ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
    return;
  }
  auto previousEntry = this->scopes->lookup(functionDecl->ownName());
  auto declaredHere = ErrorTextDeclare::previous();
  this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
}

void ClosureChecker::walkClosureBody(FunctionDecl *functionDecl) {
  auto saved = this->returnInferrer->enterBody(functionDecl);
  this->keepConstantFlow(saved.flow);
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

void ClosureChecker::keepConstantFlow(const Flow &outer) {
  auto &flowState = this->state.getFlowState();
  for (const Facts::Fact &fact : outer.facts.all()) {
    if (this->isFixedTerm(fact.lhs) && this->isFixedTerm(fact.rhs)) {
      flowState.getFacts().add(fact.lhs, fact.rhs, fact.offset);
    }
  }
  for (const VarSymbol *variable : outer.defined) {
    if (variable->isConst) {
      flowState.markDefined(variable);
    }
  }
  for (const auto &[variable, type] : outer.narrowed) {
    if (variable->isConst) {
      flowState.addNarrowed(variable, type);
    }
  }
  for (const VarSymbol *variable : outer.okResults) {
    if (variable->isConst) {
      flowState.markOkResult(variable);
    }
  }
  for (const VarSymbol *variable : outer.errResults) {
    if (variable->isConst) {
      flowState.markErrResult(variable);
    }
  }
}

bool ClosureChecker::isFixedTerm(const Term &term) const {
  bool isFirstFixed = term.var == nullptr || (term.var->isConst && !term.length);
  bool isSecondFixed = term.var2 == nullptr || (term.var2->isConst && !term.length2);

  return isFirstFixed && isSecondFixed;
}

void ClosureChecker::walkStatements(BlockStmt *block) {
  this->typeDeclarer->declareTypes(block->body);
  auto *group = this->nestedFunctions->declareNested(block);
  if (group != nullptr) {
    this->state.getFunctionStack().enterGroup(group);
  }
  auto names = this->blockNames(block);
  this->state.getScopeStack().enterBlockNames(std::move(names));
  for (size_t index = 0; index < block->body.size(); ++index) {
    if (group != nullptr && index == group->firstUse) {
      this->markGroupCreated(group);
    }
    this->state.getBlockPosition().rememberBlockPosition(block->body, index);
    this->stmtChecker->walkStmt(block->body[index]);
    if (this->isDecidedEnd(block, index, group)) {
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

BlockNames ClosureChecker::blockNames(const BlockStmt *block) const {
  auto names = BlockNames{this->state.getFunctionStack().getFunctionDepth(), {}};
  for (Stmt *stmt : block->body) {
    auto *varDecl = dyn_cast<VarDeclStmt>(stmt);
    if (varDecl != nullptr) {
      this->addDeclaratorNames(varDecl, names);
    }
  }

  return names;
}

void ClosureChecker::addDeclaratorNames(const VarDeclStmt *varDecl, BlockNames &names) const {
  for (const VarDeclarator &declarator : varDecl->decls) {
    names.names.push_back(declarator.name);
  }
}

void ClosureChecker::markGroupCreated(NestedGroup *group) {
  group->creationSeq = this->state.getReturnInference().takeSequenceNumber();
  for (FunctionDecl *functionDecl : group->members) {
    LocalVarState *localState = functionDecl->var->local();
    if (localState != nullptr) {
      localState->declSeq = group->creationSeq;
    }
  }
}

bool ClosureChecker::isDecidedEnd(const BlockStmt *block, size_t index,
                                  const NestedGroup *group) const {
  auto *decided = dyn_cast<IfStmt>(block->body[index]);
  bool isConstantIf = decided != nullptr && isa<BoolLitExpr>(decided->cond);
  bool isGroupCreated = group == nullptr || group->firstUse <= index;
  bool isEndReached = this->state.getGenericStore().isCheckingInstance() && isConstantIf &&
                      this->state.getFlowState().isUnreachable() && isGroupCreated;
  if (!isEndReached) {
    return false;
  }
  auto rest = llvm::ArrayRef<Stmt *>(block->body).drop_front(index + 1);

  return llvm::none_of(rest, std::bind_front(&ClosureChecker::isDeclarationStmt, this));
}

bool ClosureChecker::isDeclarationStmt(const Stmt *stmt) const {
  return isa<FunctionDecl>(stmt) || isa<ClassDecl>(stmt);
}

}
