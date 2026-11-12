#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/iterators/iterable_checker/iterable_checker.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/bodies/stmt/return_checker/return_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/late_classes/late_classes.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/stream/chunk_types/chunk_types.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include <cmath>

using llvm::dyn_cast;

namespace tz {

FunctionChecker::FunctionChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->byteProofs = &all.byteProofs;
  this->chunkTypes = &all.chunkTypes;
  this->typeParams = &all.typeParams;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->indexProofs = &all.indexProofs;
  this->lateClasses = &all.lateClasses;
  this->asyncChecker = &all.asyncChecker;
  this->classChecker = &all.classChecker;
  this->typeResolver = &all.typeResolver;
  this->typeRelations = &all.typeRelations;
  this->returnChecker = &all.returnChecker;
  this->argListChecker = &all.argListChecker;
  this->closureChecker = &all.closureChecker;
  this->emitterChecker = &all.emitterChecker;
  this->returnInferrer = &all.returnInferrer;
  this->iterableChecker = &all.iterableChecker;
  this->generatorChecker = &all.generatorChecker;
  this->predicateChecker = &all.predicateChecker;
  this->tzModulesChecker = &all.tzModulesChecker;
  this->genericClassChecker = &all.genericClassChecker;
}

FunctionChecker::ResultTopLevelGenerics
FunctionChecker::topLevelGeneric(const FunctionDecl *functionDecl) const {
  if (!functionDecl->typeParams.empty()) {
    auto message = ErrorTextFunctionDecl::nested();

    return err(FunctionCheckerTopLevelGenericError{
        .code = FunctionCheckerTopLevelGenericErrorCode::NESTED_GENERIC,
        .message = message,
    });
  }

  return ok(functionDecl);
}

FunctionChecker::ResultDecoratorAllowed
FunctionChecker::paramDecorators(const FunctionDecl *functionDecl, const Param &param) const {
  bool isClassMethod = functionDecl->cls() != nullptr && functionDecl->cls()->literal() == nullptr;
  if (!param.decorators.empty() && !isClassMethod && !functionDecl->isInstance) {
    auto message = ErrorTextFunctionDecl::decorator();

    return err(FunctionCheckerParamDecoratorsError{
        .code = FunctionCheckerParamDecoratorsErrorCode::DECORATOR_NOT_ALLOWED,
        .message = message,
    });
  }

  return ok(&param);
}

FunctionChecker::ResultThisParamAllowed
FunctionChecker::thisParam(const FunctionDecl *functionDecl) const {
  if (functionDecl->isArrow()) {
    auto message = ErrorTextFunctionDecl::arrowThis();

    return err(FunctionCheckerThisParamError{
        .code = FunctionCheckerThisParamErrorCode::ARROW_THIS,
        .message = message,
    });
  }
  if (functionDecl->cls() != nullptr) {
    auto message = ErrorTextFunctionDecl::methodThis();

    return err(FunctionCheckerThisParamError{
        .code = FunctionCheckerThisParamErrorCode::METHOD_THIS,
        .message = message,
    });
  }

  return ok(functionDecl);
}

FunctionChecker::ResultRestParamIsArray FunctionChecker::restType(const Type *type,
                                                                  const Param &param) const {
  bool isList = type->isArray() || type->isListParam() || type->isTuple() || type->isError();
  if (!isList) {
    auto message = ErrorTextFunctionDecl::restArray(param.name);

    return err(FunctionCheckerRestTypeError{
        .code = FunctionCheckerRestTypeErrorCode::REST_NOT_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

FunctionChecker::ResultParamOrderIsKept FunctionChecker::requiredOrder(const Param &param,
                                                                       bool afterOptional) const {
  if (afterOptional) {
    auto message = ErrorTextFunctionDecl::required();

    return err(FunctionCheckerRequiredOrderError{
        .code = FunctionCheckerRequiredOrderErrorCode::REQUIRED_AFTER_OPTIONAL,
        .message = message,
    });
  }

  return ok(&param);
}

FunctionChecker::ResultParamTypeIsKnown FunctionChecker::paramTypeKnown(const Param &param) const {
  const Expr *value = param.defaultValue();
  bool isLiteral = value != nullptr && SharedHelpers::literalType(value) != nullptr;
  if (param.type || isLiteral || param.property) {
    return ok(&param);
  }
  if (value != nullptr) {
    auto message = ErrorTextFunctionDecl::defaultNeedsType(param.label());

    return err(FunctionCheckerParamTypeKnownError{
        .code = FunctionCheckerParamTypeKnownErrorCode::DEFAULT_NOT_LITERAL,
        .message = message,
    });
  }
  auto message = ErrorTextFunctionDecl::needsType(param.label());

  return err(FunctionCheckerParamTypeKnownError{
      .code = FunctionCheckerParamTypeKnownErrorCode::NEEDS_TYPE,
      .message = message,
  });
}

void FunctionChecker::declareFunction(FunctionDecl *functionDecl, VarSymbol *variable) {
  this->declareSymbol(functionDecl, variable);
  if (!functionDecl->name.empty()) {
    this->declareFunctionName(functionDecl, functionDecl->sym, variable);
  }
}

void FunctionChecker::declareSignature(FunctionDecl *functionDecl) {
  this->declareSymbol(functionDecl, nullptr);
}

void FunctionChecker::declareSymbol(FunctionDecl *functionDecl, VarSymbol *variable) {
  auto *function = this->state.getAst().newFunc();
  function->setCompleter(this->state.getCompleter());
  function->name = functionDecl->name;
  function->loc = functionDecl->loc();
  function->decl = functionDecl;
  bool isGeneric = this->isGenericDeclaration(functionDecl);
  bool isClassParams = functionDecl->cls() != nullptr && !functionDecl->isStatic() &&
                       !functionDecl->cls()->typeParams.empty();
  if (isGeneric || isClassParams) {
    this->state.getScopeStack().openTypeScope();
  }
  if (isClassParams) {
    this->genericClassChecker->bindClassTypeParams(functionDecl->cls());
  }
  if (isGeneric) {
    this->declareFunctionTypeParams(functionDecl, function, variable);
  }
  std::optional<std::vector<const Type *>> hook =
      functionDecl->cls() != nullptr ? this->chunkTypes->streamHookParams(functionDecl)
                                     : std::nullopt;
  this->padHookParams(functionDecl, hook);
  bool isAfterOptional = false;
  for (size_t index = 0; index < functionDecl->params.size(); ++index) {
    const Type *hooked = hook.has_value() && index < hook->size() ? (*hook)[index] : nullptr;
    this->declareFunctionParam(functionDecl, function, index, hooked, isAfterOptional);
  }
  this->declareFunctionReturn(functionDecl, function);
  if (isGeneric || isClassParams) {
    this->state.getScopeStack().closeTypeScope();
  }
  this->argListChecker->expandArgumentList(functionDecl, function);
  functionDecl->sym = function;
}

bool FunctionChecker::isGenericDeclaration(const FunctionDecl *functionDecl) {
  if (!functionDecl->typeParams.empty()) {
    return true;
  }
  for (const Param &param : functionDecl->params) {
    if (param.type.has_value() && this->iterableChecker->isIterableRef(*param.type)) {
      return true;
    }
  }

  return false;
}

void FunctionChecker::declareFunctionTypeParams(FunctionDecl *functionDecl, FuncSymbol *function,
                                                VarSymbol *variable) {
  if (variable == nullptr) {
    function->typeParams =
        this->typeParams->declareTypeParams(functionDecl->typeParams, &function->typeDefaults);
    functionDecl->typeArgs = function->typeParams;
    return;
  }
  auto topLevel = this->topLevelGeneric(functionDecl);
  if (topLevel.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), topLevel.error().message);
  }
  functionDecl->typeArgs.assign(functionDecl->typeParams.size(), Type::error());
  this->typeParams->bindTypeParams(functionDecl->typeParams, functionDecl->typeArgs);
}

void FunctionChecker::padHookParams(FunctionDecl *functionDecl,
                                    const std::optional<std::vector<const Type *>> &hook) {
  if (!hook.has_value() || this->hasRestParam(functionDecl)) {
    return;
  }
  for (size_t index = functionDecl->params.size(); index < hook->size(); ++index) {
    Param hidden;
    hidden.name = this->state.getAst().hiddenName();
    hidden.loc = functionDecl->loc();
    functionDecl->params.push_back(hidden);
  }
}

bool FunctionChecker::hasRestParam(const FunctionDecl *functionDecl) const {
  for (const Param &param : functionDecl->params) {
    if (param.rest()) {
      return true;
    }
  }

  return false;
}

void FunctionChecker::declareFunctionParam(FunctionDecl *functionDecl, FuncSymbol *function,
                                           size_t index, const Type *hooked, bool &afterOptional) {
  Param &param = functionDecl->params[index];
  auto decorated = this->paramDecorators(functionDecl, param);
  if (decorated.isErr()) {
    this->state.getDiagnostics().error(param.decorators[0].loc, decorated.error().message);
  }
  const auto *type = this->declaredParamType(functionDecl, function, param, hooked);
  if (param.optional()) {
    type = Type::optional(type);
  }
  param.declaredType = type;
  if (index == 0) {
    this->walkThisParam(functionDecl, function, param);
  }
  if (param.rest()) {
    auto rest = this->restType(type, param);
    if (rest.isErr()) {
      this->state.getDiagnostics().error(param.loc, rest.error().message);
      param.declaredType = Type::error();
    }
    function->hasRest = true;
    function->params.push_back(param.declaredType);
    return;
  }
  bool isMayOmit = param.optional() || param.defaultValue() != nullptr;
  function->params.push_back(isMayOmit ? Type::optional(type) : type);
  if (!isMayOmit) {
    auto order = this->requiredOrder(param, afterOptional);
    if (order.isErr()) {
      this->state.getDiagnostics().error(param.loc, order.error().message);
    }
    function->minArgs = index + 1;
  }
  afterOptional |= param.optional();
}

const Type *FunctionChecker::declaredParamType(FunctionDecl *functionDecl, FuncSymbol *function,
                                               const Param &param, const Type *hooked) {
  if (param.type.has_value()) {
    return this->writtenParamType(functionDecl, function, param, hooked);
  }
  if (hooked != nullptr && !param.rest()) {
    return hooked;
  }
  const Type *literal =
      param.defaultValue() != nullptr ? SharedHelpers::literalType(param.defaultValue()) : nullptr;
  if (literal != nullptr) {
    return literal;
  }
  auto known = this->paramTypeKnown(param);
  if (known.isErr()) {
    this->state.getDiagnostics().error(param.loc, known.error().message);
  }

  return Type::error();
}

const Type *FunctionChecker::writtenParamType(FunctionDecl *functionDecl, FuncSymbol *function,
                                              const Param &param, const Type *hooked) {
  const Type *type = nullptr;
  auto iterable = this->iterableChecker->iterableParam(functionDecl, function, *param.type);
  if (iterable.isOk()) {
    type = iterable.value();
  } else if (iterable.error().code != IterableCheckerIterableParamErrorCode::NOT_ITERABLE) {
    this->state.getDiagnostics().error(param.type->loc, iterable.error().message);
    type = Type::error();
  }
  this->state.getTypeContext().enterListContext(param.rest());
  if (type == nullptr) {
    type = this->typeResolver->resolveType(*param.type);
  }
  this->state.getTypeContext().leaveListContext();
  if (hooked != nullptr && type != nullptr && type->isFloat() && hooked->isInt()) {
    type = hooked;
  }

  return type;
}

void FunctionChecker::walkThisParam(const FunctionDecl *functionDecl, FuncSymbol *function,
                                    const Param &param) {
  if (param.name != "this" || param.rest() || !param.type.has_value()) {
    return;
  }
  auto thisAllowed = this->thisParam(functionDecl);
  if (thisAllowed.isErr()) {
    this->state.getDiagnostics().error(param.loc, thisAllowed.error().message);
    return;
  }
  function->hasThis = !param.optional() && param.defaultValue() == nullptr;
}

void FunctionChecker::declareFunctionReturn(FunctionDecl *functionDecl, FuncSymbol *function) {
  if (functionDecl->returnType.has_value()) {
    const auto *declared = this->predicateChecker->resolveReturnType(functionDecl, function);
    auto generator = this->generatorChecker->generatorReturnType(functionDecl, declared);
    if (generator.isErr()) {
      this->state.getDiagnostics().error(functionDecl->returnType->loc, generator.error().message);
    }
    const Type *returned = generator.isOk() ? generator.value() : Type::error();
    function->setReturnType(this->asyncChecker->asyncReturnType(functionDecl, returned));
    return;
  }
  if (functionDecl->isGenerator || this->returnInferrer->isReturningValue(functionDecl->body)) {
    return;
  }
  function->setReturnType(functionDecl->isAsync ? Type::promise(Type::voidTy()) : Type::voidTy());
}

void FunctionChecker::declareFunctionName(FunctionDecl *functionDecl, FuncSymbol *function,
                                          VarSymbol *variable) {
  auto added =
      this->scopes->declare(functionDecl->name, {FunctionEntry{.func = function, .var = variable}});
  if (added.isOk()) {
    return;
  }
  this->state.getDiagnostics().error(functionDecl->loc(), added.error().message);
  if (added.error().code == ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
    return;
  }
  auto previousEntry = this->scopes->lookup(functionDecl->name);
  auto declaredHere = ErrorTextDeclare::previous();
  this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
}

FunctionChecker::ResultConstructorSuper
FunctionChecker::superCalled(const FunctionDecl *functionDecl, unsigned supers) const {
  if (supers == 0) {
    auto message = ErrorTextFunctionBody::noSuper(functionDecl->cls()->name,
                                                  functionDecl->cls()->parent->name);

    return err(FunctionCheckerSuperCalledError{
        .code = FunctionCheckerSuperCalledErrorCode::NO_SUPER_CALL,
        .message = message,
    });
  }

  return ok(functionDecl);
}

FunctionChecker::ResultDefaultUsesPrior
FunctionChecker::defaultOrder(const FunctionDecl *functionDecl, size_t index,
                              const llvm::StringSet<> &names) const {
  for (size_t j = index; j < functionDecl->params.size(); ++j) {
    if (names.contains(functionDecl->params[j].name)) {
      auto message = ErrorTextFunctionBody::forward(functionDecl->params[index].label(),
                                                    functionDecl->params[j].name);

      return err(FunctionCheckerDefaultOrderError{
          .code = FunctionCheckerDefaultOrderErrorCode::FORWARD_REFERENCE,
          .message = message,
      });
    }
  }

  return ok(functionDecl);
}

FunctionChecker::ResultEveryPathReturns
FunctionChecker::returnsEverywhere(const FunctionDecl *functionDecl, const Type *returnType) const {
  bool isMissing = returnType != nullptr && !returnType->isVoid() && !returnType->isError() &&
                   !returnType->isOptional() && !returnType->isUnknown() &&
                   !this->returnChecker->alwaysReturns(functionDecl->body);
  if (isMissing) {
    auto label = ErrorTextFunctionName::label(functionDecl->name);
    auto message = ErrorTextFunctionBody::noReturn(label, returnType->name());

    return err(FunctionCheckerReturnsEverywhereError{
        .code = FunctionCheckerReturnsEverywhereErrorCode::MISSING_RETURN,
        .message = message,
    });
  }

  return ok(functionDecl);
}

void FunctionChecker::walkFunctionBody(FunctionDecl *functionDecl) {
  if (functionDecl->intrinsic()) {
    functionDecl->checked = true;
    return;
  }
  if (functionDecl->external()) {
    return this->tzModulesChecker->walkExternal(functionDecl);
  }
  FuncSymbol *function = functionDecl->sym;
  functionDecl->checked = true;
  auto &functions = this->state.getFunctionStack();
  auto *outer = functions.getCurrentFunction();
  functions.rememberCurrentFunction(function);
  functions.enterFunction(functionDecl);
  this->state.getDiagnostics().pushList(DiagnosticRoute::PARENT);
  auto &classContext = this->state.getClassContext();
  classContext.enterClass(functionDecl->isClosure() ? classContext.getCurrentClass()
                                                    : functionDecl->cls());
  this->state.getScopeStack().openScope();
  this->bindBodyTypeParams(functionDecl);
  bool isInitializer = functionDecl->takesThis() && this->entersInitializer(functionDecl);
  for (size_t index = 0; index < functionDecl->params.size(); ++index) {
    this->declareBodyParam(functionDecl, index);
  }
  this->walkBodyStatements(functionDecl);
  this->scopes->closeScope();
  functions.leaveFunction();
  functions.rememberCurrentFunction(outer);
  if (isInitializer) {
    classContext.leaveInitializer();
  }
  classContext.leaveClass();
  const auto *returnType = this->bodyReturnType(functionDecl);
  auto returns = this->returnsEverywhere(functionDecl, returnType);
  if (returns.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), returns.error().message);
  }
  auto bodyDiagnostics = this->state.getDiagnostics().popList();
  auto destination = this->state.getDiagnostics().getCurrentDestination();
  bool isReachingProgram = destination == DiagnosticRoute::PROGRAM;
  bool hasFailed = Diagnostics::hasErrors(bodyDiagnostics) && isReachingProgram;
  if (function->isGeneric() && hasFailed) {
    this->state.getGenericStore().markBrokenGeneric(function);
  }
  if (functionDecl->cls() != nullptr && functionDecl->cls()->isGeneric() && hasFailed) {
    this->state.getClassStore().markBrokenClass(functionDecl->cls());
  }
}

void FunctionChecker::bindBodyTypeParams(FunctionDecl *functionDecl) {
  if (functionDecl->cls() != nullptr && !functionDecl->isStatic()) {
    this->genericClassChecker->bindClassTypeParams(functionDecl->cls());
  }
  if (functionDecl->cls() != nullptr) {
    this->lateClasses->bindOuterTypeParams(functionDecl->cls());
  }
  if (!functionDecl->typeArgs.empty()) {
    this->typeParams->bindTypeParams(functionDecl->typeParams, functionDecl->typeArgs);
  }
}

bool FunctionChecker::entersInitializer(FunctionDecl *functionDecl) {
  auto declared = this->classChecker->declareThis(functionDecl);
  if (declared.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), declared.error().message);
  }
  VarSymbol *self = declared.isOk() ? declared.value() : nullptr;
  if (!functionDecl->isConstructor() && functionDecl != functionDecl->cls()->init) {
    return false;
  }
  this->state.getClassContext().enterInitializer(functionDecl, self);
  this->state.getFlowState().forgetSetFields();
  bool isDerived = functionDecl->cls()->parent != nullptr;
  for (const ClassField &field : functionDecl->cls()->fields) {
    if (this->isPresetField(functionDecl, field, isDerived)) {
      this->state.getFlowState().addSetField(field.name);
    }
  }
  if (isDerived && functionDecl->isConstructor()) {
    this->state.getFlowState().markBeforeSuper();
  } else {
    this->state.getFlowState().markSuperCalled();
  }
  if (this->state.getFlowState().isBeforeSuper()) {
    this->walkRootSupers(functionDecl);
  }

  return true;
}

bool FunctionChecker::isPresetField(const FunctionDecl *functionDecl, const ClassField &field,
                                    bool derived) const {
  if (field.isStatic()) {
    return false;
  }
  bool isOwnInit = functionDecl->isConstructor() && !derived && field.init != nullptr &&
                   field.method() == nullptr;

  return field.optional() || isOwnInit || (!functionDecl->isConstructor() && field.redeclared);
}

void FunctionChecker::walkRootSupers(FunctionDecl *functionDecl) {
  unsigned supers = 0;
  for (Stmt *stmt : functionDecl->body->body) {
    auto *call = this->superCallOf(stmt);
    if (call != nullptr) {
      this->state.getClassStore().markRootSuper(call);
      ++supers;
    }
  }
  auto called = this->superCalled(functionDecl, supers);
  if (called.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), called.error().message);
  }
}

CallExpr *FunctionChecker::superCallOf(Stmt *stmt) const {
  auto *expr = dyn_cast<ExprStmt>(stmt);
  auto *call = expr != nullptr ? dyn_cast<CallExpr>(expr->expr) : nullptr;
  auto *callee = call != nullptr ? dyn_cast<NameExpr>(call->callee) : nullptr;
  if (callee == nullptr || callee->name != "super") {
    return nullptr;
  }

  return call;
}

void FunctionChecker::declareBodyParam(FunctionDecl *functionDecl, size_t index) {
  Param &param = functionDecl->params[index];
  const Type *type = param.declaredType;
  bool isUntyped = param.defaultValue() != nullptr && type->isError() && !param.type.has_value();
  if (isUntyped) {
    param.defaultValue()->type = type;
    param.sym = this->scopes->declareLet(param.name, param.loc, type);
    return;
  }
  auto *defaulted = param.form.as<DefaultedParam>();
  if (defaulted != nullptr && !this->defaultValueChecked(functionDecl, index, defaulted, type)) {
    param.sym = this->scopes->declareLet(param.name, param.loc, type);
    return;
  }
  param.sym = this->scopes->declareLet(param.name, param.loc, type);
  Expr *given = param.defaultValue();
  bool isDefined = given != nullptr && type->isOptional() && !given->type->isOptional() &&
                   !given->type->isUndefined();
  if (isDefined) {
    this->flowChecker->markDefined(param.sym);
  }
}

bool FunctionChecker::defaultValueChecked(FunctionDecl *functionDecl, size_t index,
                                          DefaultedParam *defaulted, const Type *type) {
  auto names = llvm::StringSet<>();
  AstWalker::collectNames(defaulted->defaultValue, names);
  auto prior = this->defaultOrder(functionDecl, index, names);
  if (prior.isErr()) {
    this->state.getDiagnostics().error(defaulted->defaultValue->loc(), prior.error().message);
    defaulted->defaultValue->type = Type::error();
    return false;
  }
  auto *functionExpr = dyn_cast<FunctionExpr>(defaulted->defaultValue);
  if (functionExpr != nullptr && functionExpr->fn->name.empty()) {
    functionExpr->fn->name = functionDecl->params[index].name;
  }
  auto *checked =
      this->exprChecker->checkExpr(defaulted->defaultValue, type->isError() ? nullptr : type);
  defaulted->defaultValue = this->typeRelations->coerce(checked, type);

  return true;
}

void FunctionChecker::walkBodyStatements(FunctionDecl *functionDecl) {
  this->byteProofs->addByteCallbackFacts(functionDecl);
  this->indexProofs->addIndexCallbackFacts(functionDecl);
  this->argListChecker->declareArgumentList(functionDecl);
  this->predicateChecker->beginPredicate(functionDecl);
  this->state.getFlowState().enterErrorWatch();
  this->closureChecker->walkStatements(functionDecl->body);
  if (!this->state.getFlowState().isUnreachable()) {
    this->emitterChecker->reportUnlistenedErrors();
  }
  this->state.getFlowState().leaveErrorWatch();
  this->predicateChecker->endPredicate(functionDecl);
  bool isConstructor = functionDecl->isConstructor() &&
                       this->state.getClassContext().getInitializer() == functionDecl &&
                       !this->state.getFlowState().isBeforeSuper();
  if (!isConstructor) {
    return;
  }
  auto fieldsSet = this->classChecker->checkFieldsSet();
  if (fieldsSet.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), fieldsSet.error().message);
  }
}

const Type *FunctionChecker::bodyReturnType(const FunctionDecl *functionDecl) const {
  FuncSymbol *function = functionDecl->sym;
  bool isPromised = functionDecl->isAsync && function->hasReturnType() &&
                    function->getKnownReturnType()->isPromise();
  if (isPromised) {
    return function->getKnownReturnType()->promised();
  }
  if (!functionDecl->isGenerator) {
    return function->getKnownReturnType();
  }
  bool isIterating =
      function->hasReturnType() && (function->getKnownReturnType()->isIterator() ||
                                    function->getKnownReturnType()->isAsyncIterator());

  return isIterating ? function->getKnownReturnType()->returned() : nullptr;
}

void FunctionChecker::walkFunctionFromScratch(FunctionDecl *functionDecl) {
  this->state.getFlowState().replaceFlow(Flow());
  this->walkFunctionBody(functionDecl);
}

}
