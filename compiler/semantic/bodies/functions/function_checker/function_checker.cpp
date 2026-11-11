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
  this->scopes = &all.getScopes();
  this->byteProofs = &all.getByteProofs();
  this->chunkTypes = &all.getChunkTypes();
  this->typeParams = &all.getTypeParams();
  this->exprChecker = &all.getExprChecker();
  this->flowChecker = &all.getFlowChecker();
  this->indexProofs = &all.getIndexProofs();
  this->lateClasses = &all.getLateClasses();
  this->asyncChecker = &all.getAsyncChecker();
  this->classChecker = &all.getClassChecker();
  this->typeResolver = &all.getTypeResolver();
  this->typeRelations = &all.getTypeRelations();
  this->argListChecker = &all.getArgListChecker();
  this->closureChecker = &all.getClosureChecker();
  this->emitterChecker = &all.getEmitterChecker();
  this->iterableChecker = &all.getIterableChecker();
  this->generatorChecker = &all.getGeneratorChecker();
  this->predicateChecker = &all.getPredicateChecker();
  this->tzModulesChecker = &all.getTzModulesChecker();
  this->genericClassChecker = &all.getGenericClassChecker();
}

FunctionChecker::ResultTopLevelGenerics FunctionChecker::topLevelGeneric(const FunctionDecl *fn) {
  if (!fn->typeParams.empty()) {
    std::string message = ErrorTextFunctionDecl::nested();

    return err(FunctionCheckerTopLevelGenericError{
        .code = FunctionCheckerTopLevelGenericErrorCode::NESTED_GENERIC,
        .message = message,
    });
  }

  return ok(fn);
}

FunctionChecker::ResultDecoratorAllowed FunctionChecker::paramDecorators(const FunctionDecl *fn,
                                                                         const Param &param) {
  bool classMethod = fn->cls() && !fn->cls()->literal();
  if (!param.decorators.empty() && !classMethod && !fn->isInstance) {
    std::string message = ErrorTextFunctionDecl::decorator();

    return err(FunctionCheckerParamDecoratorsError{
        .code = FunctionCheckerParamDecoratorsErrorCode::DECORATOR_NOT_ALLOWED,
        .message = message,
    });
  }

  return ok(&param);
}

FunctionChecker::ResultThisParamAllowed FunctionChecker::thisParam(const FunctionDecl *fn) {
  if (fn->isArrow()) {
    std::string message = ErrorTextFunctionDecl::arrowThis();

    return err(FunctionCheckerThisParamError{
        .code = FunctionCheckerThisParamErrorCode::ARROW_THIS,
        .message = message,
    });
  }
  if (fn->cls()) {
    std::string message = ErrorTextFunctionDecl::methodThis();

    return err(FunctionCheckerThisParamError{
        .code = FunctionCheckerThisParamErrorCode::METHOD_THIS,
        .message = message,
    });
  }

  return ok(fn);
}

FunctionChecker::ResultRestParamIsArray FunctionChecker::restType(const Type *type,
                                                                  const Param &param) {
  bool list = type->isArray() || type->isListParam() || type->isTuple() || type->isError();
  if (!list) {
    std::string message = ErrorTextFunctionDecl::restArray(param.name);

    return err(FunctionCheckerRestTypeError{
        .code = FunctionCheckerRestTypeErrorCode::REST_NOT_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

FunctionChecker::ResultParamOrderIsKept FunctionChecker::requiredOrder(const Param &param,
                                                                       bool afterOptional) {
  if (afterOptional) {
    std::string message = ErrorTextFunctionDecl::required();

    return err(FunctionCheckerRequiredOrderError{
        .code = FunctionCheckerRequiredOrderErrorCode::REQUIRED_AFTER_OPTIONAL,
        .message = message,
    });
  }

  return ok(&param);
}

FunctionChecker::ResultParamTypeIsKnown FunctionChecker::paramTypeKnown(const Param &param) {
  const Expr *value = param.defaultValue();
  bool literal = value != nullptr && SharedHelpers::literalType(value) != nullptr;
  if (param.type || literal || param.property) {
    return ok(&param);
  }
  if (value != nullptr) {
    std::string message = ErrorTextFunctionDecl::defaultNeedsType(param.label());

    return err(FunctionCheckerParamTypeKnownError{
        .code = FunctionCheckerParamTypeKnownErrorCode::DEFAULT_NOT_LITERAL,
        .message = message,
    });
  }
  std::string message = ErrorTextFunctionDecl::needsType(param.label());

  return err(FunctionCheckerParamTypeKnownError{
      .code = FunctionCheckerParamTypeKnownErrorCode::NEEDS_TYPE,
      .message = message,
  });
}

void FunctionChecker::declareFunction(FunctionDecl *fn, VarSymbol *var, bool scoped) {
  FuncSymbol *sym = this->state.getAst().newFunc();
  sym->setCompleter(this->state.getCompleter());
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  bool generic = this->isGenericDeclaration(fn);
  bool classParams = fn->cls() != nullptr && !fn->isStatic() && !fn->cls()->typeParams.empty();
  if (generic || classParams) {
    this->state.getScopeStack().openTypeScope();
  }
  if (classParams) {
    this->genericClassChecker->bindClassTypeParams(fn->cls());
  }
  if (generic) {
    this->declareFunctionTypeParams(fn, sym, var);
  }
  std::optional<std::vector<const Type *>> hook =
      fn->cls() != nullptr ? this->chunkTypes->streamHookParams(fn) : std::nullopt;
  this->padHookParams(fn, hook);
  bool afterOptional = false;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const Type *hooked = hook.has_value() && i < hook->size() ? (*hook)[i] : nullptr;
    this->declareFunctionParam(fn, sym, i, hooked, afterOptional);
  }
  this->declareFunctionReturn(fn, sym);
  if (generic || classParams) {
    this->state.getScopeStack().closeTypeScope();
  }
  this->argListChecker->expandArgumentList(fn, sym);
  fn->sym = sym;
  if (!fn->name.empty() && scoped) {
    this->declareFunctionName(fn, sym, var);
  }
}

bool FunctionChecker::isGenericDeclaration(const FunctionDecl *fn) {
  if (!fn->typeParams.empty()) {
    return true;
  }
  for (const Param &p : fn->params) {
    if (p.type.has_value() && this->iterableChecker->isIterableRef(*p.type)) {
      return true;
    }
  }

  return false;
}

void FunctionChecker::declareFunctionTypeParams(FunctionDecl *fn, FuncSymbol *sym, VarSymbol *var) {
  if (var == nullptr) {
    sym->typeParams = this->typeParams->declareTypeParams(fn->typeParams, &sym->typeDefaults);
    fn->typeArgs = sym->typeParams;
    return;
  }
  ResultTopLevelGenerics topLevel = FunctionChecker::topLevelGeneric(fn);
  if (topLevel.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), topLevel.error().message);
  }
  fn->typeArgs.assign(fn->typeParams.size(), Type::error());
  this->typeParams->bindTypeParams(fn->typeParams, fn->typeArgs);
}

void FunctionChecker::padHookParams(FunctionDecl *fn,
                                    const std::optional<std::vector<const Type *>> &hook) {
  if (!hook.has_value() || FunctionChecker::hasRestParam(fn)) {
    return;
  }
  for (size_t i = fn->params.size(); i < hook->size(); ++i) {
    Param hidden;
    hidden.name = this->state.getAst().hiddenName();
    hidden.loc = fn->loc();
    fn->params.push_back(hidden);
  }
}

bool FunctionChecker::hasRestParam(const FunctionDecl *fn) {
  for (const Param &p : fn->params) {
    if (p.rest()) {
      return true;
    }
  }

  return false;
}

void FunctionChecker::declareFunctionParam(FunctionDecl *fn, FuncSymbol *sym, size_t i,
                                           const Type *hooked, bool &afterOptional) {
  Param &param = fn->params[i];
  ResultDecoratorAllowed decorated = FunctionChecker::paramDecorators(fn, param);
  if (decorated.isErr()) {
    this->state.getDiagnostics().error(param.decorators[0].loc, decorated.error().message);
  }
  const Type *type = this->declaredParamType(fn, sym, param, hooked);
  if (param.optional()) {
    type = Type::optional(type);
  }
  param.declaredType = type;
  if (i == 0) {
    this->walkThisParam(fn, sym, param);
  }
  if (param.rest()) {
    ResultRestParamIsArray rest = FunctionChecker::restType(type, param);
    if (rest.isErr()) {
      this->state.getDiagnostics().error(param.loc, rest.error().message);
      param.declaredType = Type::error();
    }
    sym->hasRest = true;
    sym->params.push_back(param.declaredType);
    return;
  }
  bool mayOmit = param.optional() || param.defaultValue();
  sym->params.push_back(mayOmit ? Type::optional(type) : type);
  if (!mayOmit) {
    ResultParamOrderIsKept order = FunctionChecker::requiredOrder(param, afterOptional);
    if (order.isErr()) {
      this->state.getDiagnostics().error(param.loc, order.error().message);
    }
    sym->minArgs = i + 1;
  }
  afterOptional |= param.optional();
}

const Type *FunctionChecker::declaredParamType(FunctionDecl *fn, FuncSymbol *sym,
                                               const Param &param, const Type *hooked) {
  if (param.type.has_value()) {
    return this->writtenParamType(fn, sym, param, hooked);
  }
  if (hooked != nullptr && !param.rest()) {
    return hooked;
  }
  const Type *literal =
      param.defaultValue() ? SharedHelpers::literalType(param.defaultValue()) : nullptr;
  if (literal != nullptr) {
    return literal;
  }
  ResultParamTypeIsKnown known = FunctionChecker::paramTypeKnown(param);
  if (known.isErr()) {
    this->state.getDiagnostics().error(param.loc, known.error().message);
  }

  return Type::error();
}

const Type *FunctionChecker::writtenParamType(FunctionDecl *fn, FuncSymbol *sym, const Param &param,
                                              const Type *hooked) {
  const Type *type = nullptr;
  IterableChecker::ResultIterableParam iterable =
      this->iterableChecker->iterableParam(fn, sym, *param.type);
  if (iterable.isOk()) {
    type = iterable.value();
  } else if (iterable.error().code != IterableCheckerIterableParamErrorCode::NOT_ITERABLE) {
    this->state.getDiagnostics().error(param.type->loc, iterable.error().message);
    type = Type::error();
  }
  this->state.getTypeContext().enterListContext(param.rest());
  if (type == nullptr) {
    type = this->typeResolver->resolveType(*param.type, false);
  }
  this->state.getTypeContext().leaveListContext();
  if (hooked != nullptr && type != nullptr && type->isFloat() && hooked->isInt()) {
    type = hooked;
  }

  return type;
}

void FunctionChecker::walkThisParam(const FunctionDecl *fn, FuncSymbol *sym, const Param &param) {
  if (param.name != "this" || param.rest() || !param.type.has_value()) {
    return;
  }
  ResultThisParamAllowed thisAllowed = FunctionChecker::thisParam(fn);
  if (thisAllowed.isErr()) {
    this->state.getDiagnostics().error(param.loc, thisAllowed.error().message);
    return;
  }
  sym->hasThis = !param.optional() && !param.defaultValue();
}

void FunctionChecker::declareFunctionReturn(FunctionDecl *fn, FuncSymbol *sym) {
  if (fn->returnType.has_value()) {
    const Type *declared = this->predicateChecker->resolveReturnType(fn, sym);
    GeneratorChecker::ResultGeneratorReturns generator =
        this->generatorChecker->generatorReturnType(fn, declared);
    if (generator.isErr()) {
      this->state.getDiagnostics().error(fn->returnType->loc, generator.error().message);
    }
    const Type *returned = generator.isOk() ? generator.value() : Type::error();
    sym->setReturnType(this->asyncChecker->asyncReturnType(fn, returned));
    return;
  }
  if (fn->isGenerator || ReturnInferrer::returnsValue(fn->body)) {
    return;
  }
  sym->setReturnType(fn->isAsync ? Type::promise(Type::voidTy()) : Type::voidTy());
}

void FunctionChecker::declareFunctionName(FunctionDecl *fn, FuncSymbol *sym, VarSymbol *var) {
  Scopes::ResultEntryDeclared added =
      this->scopes->declare(fn->name, {FunctionEntry{.func = sym, .var = var}});
  if (added.isOk()) {
    return;
  }
  this->state.getDiagnostics().error(fn->loc(), added.error().message);
  if (added.error().code == ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
    return;
  }
  ScopeEntry previousEntry = this->scopes->lookup(fn->name);
  std::string declaredHere = ErrorTextDeclare::previous();
  this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
}

FunctionChecker::ResultConstructorSuper FunctionChecker::superCalled(const FunctionDecl *fn,
                                                                     unsigned supers) {
  if (supers == 0) {
    std::string message = ErrorTextFunctionBody::noSuper(fn->cls()->name, fn->cls()->parent->name);

    return err(FunctionCheckerSuperCalledError{
        .code = FunctionCheckerSuperCalledErrorCode::NO_SUPER_CALL,
        .message = message,
    });
  }

  return ok(fn);
}

FunctionChecker::ResultDefaultUsesPrior
FunctionChecker::defaultOrder(const FunctionDecl *fn, size_t i, const llvm::StringSet<> &names) {
  for (size_t j = i; j < fn->params.size(); ++j) {
    if (names.contains(fn->params[j].name)) {
      std::string message =
          ErrorTextFunctionBody::forward(fn->params[i].label(), fn->params[j].name);

      return err(FunctionCheckerDefaultOrderError{
          .code = FunctionCheckerDefaultOrderErrorCode::FORWARD_REFERENCE,
          .message = message,
      });
    }
  }

  return ok(fn);
}

FunctionChecker::ResultEveryPathReturns FunctionChecker::returnsEverywhere(const FunctionDecl *fn,
                                                                           const Type *ret) {
  bool missing = ret && !ret->isVoid() && !ret->isError() && !ret->isOptional() &&
                 !ret->isUnknown() && !ReturnChecker::alwaysReturns(fn->body);
  if (missing) {
    std::string label = ErrorTextFunctionName::label(fn->name);
    std::string message = ErrorTextFunctionBody::noReturn(label, ret->name());

    return err(FunctionCheckerReturnsEverywhereError{
        .code = FunctionCheckerReturnsEverywhereErrorCode::MISSING_RETURN,
        .message = message,
    });
  }

  return ok(fn);
}

void FunctionChecker::checkFunctionBody(FunctionDecl *fn) {
  if (fn->intrinsic()) {
    fn->checked = true;
    return;
  }
  if (fn->external()) {
    return this->tzModulesChecker->checkExternal(fn);
  }
  FuncSymbol *sym = fn->sym;
  fn->checked = true;
  FunctionStack &functions = this->state.getFunctionStack();
  FuncSymbol *outer = functions.getCurrentFunction();
  functions.rememberCurrentFunction(sym);
  functions.enterFunction(fn);
  unsigned errors = this->state.getDiagnostics().errorCount();
  ClassContext &classContext = this->state.getClassContext();
  classContext.enterClass(fn->isClosure() ? classContext.getCurrentClass() : fn->cls());
  this->state.getScopeStack().openScope();
  this->bindBodyTypeParams(fn);
  bool initializer = fn->takesThis() && this->entersInitializer(fn);
  for (size_t i = 0; i < fn->params.size(); ++i) {
    this->declareBodyParam(fn, i);
  }
  this->walkBodyStatements(fn);
  this->scopes->closeScope();
  functions.leaveFunction();
  functions.rememberCurrentFunction(outer);
  if (initializer) {
    classContext.leaveInitializer();
  }
  classContext.leaveClass();
  const Type *ret = FunctionChecker::bodyReturnType(fn);
  ResultEveryPathReturns returns = FunctionChecker::returnsEverywhere(fn, ret);
  if (returns.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), returns.error().message);
  }
  bool failed = this->state.getDiagnostics().errorCount() > errors;
  if (sym->isGeneric() && failed) {
    this->state.getGenericStore().markBrokenGeneric(sym);
  }
  if (fn->cls() != nullptr && fn->cls()->isGeneric() && failed) {
    this->state.getClassStore().markBrokenClass(fn->cls());
  }
}

void FunctionChecker::bindBodyTypeParams(FunctionDecl *fn) {
  if (fn->cls() != nullptr && !fn->isStatic()) {
    this->genericClassChecker->bindClassTypeParams(fn->cls());
  }
  if (fn->cls() != nullptr) {
    this->lateClasses->bindOuterTypeParams(fn->cls());
  }
  if (!fn->typeArgs.empty()) {
    this->typeParams->bindTypeParams(fn->typeParams, fn->typeArgs);
  }
}

bool FunctionChecker::entersInitializer(FunctionDecl *fn) {
  ClassChecker::ResultNewThisSymbol declared = this->classChecker->declareThis(fn);
  if (declared.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), declared.error().message);
  }
  VarSymbol *self = declared.isOk() ? declared.value() : nullptr;
  if (!fn->isConstructor() && fn != fn->cls()->init) {
    return false;
  }
  this->state.getClassContext().enterInitializer(fn, self);
  this->state.getFlowState().forgetSetFields();
  bool derived = fn->cls()->parent != nullptr;
  for (const ClassField &f : fn->cls()->fields) {
    if (FunctionChecker::isPresetField(fn, f, derived)) {
      this->state.getFlowState().addSetField(f.name);
    }
  }
  if (derived && fn->isConstructor()) {
    this->state.getFlowState().markBeforeSuper();
  } else {
    this->state.getFlowState().markSuperCalled();
  }
  if (this->state.getFlowState().isBeforeSuper()) {
    this->walkRootSupers(fn);
  }

  return true;
}

bool FunctionChecker::isPresetField(const FunctionDecl *fn, const ClassField &f, bool derived) {
  if (f.isStatic()) {
    return false;
  }
  bool ownInit = fn->isConstructor() && !derived && f.init && !f.method();

  return f.optional() || ownInit || (!fn->isConstructor() && f.redeclared);
}

void FunctionChecker::walkRootSupers(FunctionDecl *fn) {
  unsigned supers = 0;
  for (Stmt *stmt : fn->body->body) {
    CallExpr *call = FunctionChecker::superCallOf(stmt);
    if (call != nullptr) {
      this->state.getClassStore().markRootSuper(call);
      ++supers;
    }
  }
  ResultConstructorSuper called = FunctionChecker::superCalled(fn, supers);
  if (called.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), called.error().message);
  }
}

CallExpr *FunctionChecker::superCallOf(Stmt *stmt) {
  auto *expr = dyn_cast<ExprStmt>(stmt);
  auto *call = expr != nullptr ? dyn_cast<CallExpr>(expr->expr) : nullptr;
  auto *callee = call != nullptr ? dyn_cast<NameExpr>(call->callee) : nullptr;
  if (callee == nullptr || callee->name != "super") {
    return nullptr;
  }

  return call;
}

void FunctionChecker::declareBodyParam(FunctionDecl *fn, size_t i) {
  Param &param = fn->params[i];
  const Type *type = param.declaredType;
  bool untyped = param.defaultValue() && type->isError() && !param.type.has_value();
  if (untyped) {
    param.defaultValue()->type = type;
    param.sym = this->scopes->declareVar(param.name, param.loc, type, false);
    return;
  }
  auto *defaulted = param.form.as<DefaultedParam>();
  if (defaulted != nullptr && !this->defaultValueChecked(fn, i, defaulted, type)) {
    param.sym = this->scopes->declareVar(param.name, param.loc, type, false);
    return;
  }
  param.sym = this->scopes->declareVar(param.name, param.loc, type, false);
  Expr *given = param.defaultValue();
  bool defined = given != nullptr && type->isOptional() && !given->type->isOptional() &&
                 !given->type->isUndefined();
  if (defined) {
    this->flowChecker->setDefined(param.sym, true);
  }
}

bool FunctionChecker::defaultValueChecked(FunctionDecl *fn, size_t i, DefaultedParam *defaulted,
                                          const Type *type) {
  llvm::StringSet<> names;
  AstWalker::collectNames(defaulted->defaultValue, names);
  ResultDefaultUsesPrior prior = FunctionChecker::defaultOrder(fn, i, names);
  if (prior.isErr()) {
    this->state.getDiagnostics().error(defaulted->defaultValue->loc(), prior.error().message);
    defaulted->defaultValue->type = Type::error();
    return false;
  }
  auto *fe = dyn_cast<FunctionExpr>(defaulted->defaultValue);
  if (fe != nullptr && fe->fn->name.empty()) {
    fe->fn->name = fn->params[i].name;
  }
  Expr *checked =
      this->exprChecker->checkExpr(defaulted->defaultValue, type->isError() ? nullptr : type);
  defaulted->defaultValue = this->typeRelations->coerce(checked, type);

  return true;
}

void FunctionChecker::walkBodyStatements(FunctionDecl *fn) {
  this->byteProofs->addByteCallbackFacts(fn);
  this->indexProofs->addIndexCallbackFacts(fn);
  this->argListChecker->declareArgumentList(fn);
  this->predicateChecker->beginPredicate(fn);
  this->state.getFlowState().enterErrorWatch();
  this->closureChecker->checkStatements(fn->body);
  if (!this->state.getFlowState().isUnreachable()) {
    this->emitterChecker->reportUnlistenedErrors();
  }
  this->state.getFlowState().leaveErrorWatch();
  this->predicateChecker->endPredicate(fn);
  bool constructor = fn->isConstructor() && this->state.getClassContext().getInitializer() == fn &&
                     !this->state.getFlowState().isBeforeSuper();
  if (!constructor) {
    return;
  }
  ClassChecker::ResultEveryFieldSet fieldsSet = this->classChecker->checkFieldsSet();
  if (fieldsSet.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), fieldsSet.error().message);
  }
}

const Type *FunctionChecker::bodyReturnType(const FunctionDecl *fn) {
  FuncSymbol *sym = fn->sym;
  bool promised = fn->isAsync && sym->hasReturnType() && sym->getKnownReturnType()->isPromise();
  if (promised) {
    return sym->getKnownReturnType()->promised();
  }
  if (!fn->isGenerator) {
    return sym->getKnownReturnType();
  }
  bool iterates = sym->hasReturnType() && (sym->getKnownReturnType()->isIterator() ||
                                           sym->getKnownReturnType()->isAsyncIterator());

  return iterates ? sym->getKnownReturnType()->returned() : nullptr;
}

void FunctionChecker::walkFunctionFromScratch(FunctionDecl *fn) {
  this->state.getFlowState().replaceFlow(Flow());
  this->checkFunctionBody(fn);
}

}
