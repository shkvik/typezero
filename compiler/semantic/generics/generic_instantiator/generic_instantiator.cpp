#include "compiler/semantic/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/semantic/generics/inference/inference.h"
#include "compiler/semantic/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/late_classes/late_classes.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr unsigned kMaxInstanceChain = 16;

}

GenericInstantiator::GenericInstantiator(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeParams = &all.typeParams;
  this->lateClasses = &all.lateClasses;
  this->typeResolver = &all.typeResolver;
  this->typeRelations = &all.typeRelations;
  this->argListChecker = &all.argListChecker;
  this->returnInferrer = &all.returnInferrer;
  this->functionChecker = &all.functionChecker;
  this->typeArgInferrer = &all.typeArgInferrer;
  this->genericClassChecker = &all.genericClassChecker;
}

const ClassDecl *GenericInstantiator::literalClassIn(const Type *type) const {
  auto parts = TypeParts::collect(type, kLiteralClassRoute);
  for (const Type *part : parts.getTypes()) {
    const ClassDecl *classDecl = part->classDecl();
    if (classDecl != nullptr && classDecl->literal() != nullptr) {
      return classDecl;
    }
  }

  return nullptr;
}

GenericInstantiator::ResultTypeArgsCount
GenericInstantiator::typeArgCount(const FuncSymbol *generic, const NameExpr *name) const {
  if (name->typeArgs.size() > generic->typeParams.size()) {
    auto message = ErrorTextGenericCount::tooMany(generic->name, generic->typeParams.size(),
                                                  name->typeArgs.size());

    return err(GenericInstantiatorTypeArgCountError{
        .code = GenericInstantiatorTypeArgCountErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(name->typeArgs.size());
}

GenericInstantiator::ResultTypeArgsGiven
GenericInstantiator::typeArgsGiven(const FuncSymbol *generic, const NameExpr *name,
                                   const Signature *expected) const {
  if (name->typeArgs.empty() && expected == nullptr) {
    auto message = ErrorTextGenericValue::needsArgs(generic->name);

    return err(GenericInstantiatorTypeArgsGivenError{
        .code = GenericInstantiatorTypeArgsGivenErrorCode::NEEDS_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(name);
}

GenericInstantiator::ResultNoClassReturn
GenericInstantiator::ownClassReturn(const FuncSymbol *generic, const ClassDecl *classDecl) const {
  if (classDecl != nullptr) {
    auto message = ErrorTextOwnClass::literal(generic->name);

    return err(GenericInstantiatorOwnClassReturnError{
        .code = GenericInstantiatorOwnClassReturnErrorCode::RETURNS_LITERAL_CLASS,
        .message = message,
    });
  }

  return ok(generic);
}

GenericInstantiator::ResultChainIsFinite
GenericInstantiator::chainBounded(const FuncSymbol *generic,
                                  llvm::ArrayRef<const Type *> args) const {
  unsigned chain = this->chainLength(generic);
  if (chain >= kMaxInstanceChain) {
    auto types = GenericCheckHelpers::typeNames(args, ", ");
    auto message = ErrorTextInstanceChain::endless(generic->name, types);

    return err(GenericInstantiatorChainBoundedError{
        .code = GenericInstantiatorChainBoundedErrorCode::ENDLESS_CHAIN,
        .message = message,
    });
  }

  return ok(chain);
}

unsigned GenericInstantiator::chainLength(const FuncSymbol *generic) const {
  unsigned chain = 0;
  const InstanceRecord *record = this->state.getGenericStore().getCheckingInstance();
  while (record != nullptr) {
    if (record->generic == generic) {
      chain += 1;
    }
    record = record->requester;
  }

  return chain;
}

GenericInstantiator::ResultParamsMeaning
GenericInstantiator::paramsMeaningful(const FuncSymbol *generic, llvm::ArrayRef<const Type *> reps,
                                      llvm::ArrayRef<const Type *> args) {
  for (const Type *type : generic->params) {
    const Type *substituted =
        type != nullptr ? this->typeParams->substitute(type, generic->typeParams, reps) : nullptr;
    if (substituted != nullptr && substituted->isError()) {
      auto bindings = GenericCheckHelpers::bindingNames(generic->typeParams, args);
      auto message = ErrorTextInstanceParams::meaningless(generic->name, bindings, type->name());

      return err(GenericInstantiatorParamsMeaningfulError{
          .code = GenericInstantiatorParamsMeaningfulErrorCode::PARAMS_MEANINGLESS,
          .message = message,
      });
    }
  }

  return ok(generic);
}

Expr *GenericInstantiator::checkGenericValue(NameExpr *name, FuncSymbol *generic,
                                             const Signature *expected) {
  name->type = Type::error();
  const auto *declaredReturn = generic->getReturnType();
  bool isOwnClass = this->returnsOwnClass(generic, declaredReturn);
  const Type *returnType = isOwnClass ? nullptr : declaredReturn;
  std::vector<const Type *> bound(generic->typeParams.size(), nullptr);
  auto given = this->typeArgsGiven(generic, name, expected);
  if (given.isErr()) {
    this->state.getDiagnostics().error(name->loc(), given.error().message);

    return name;
  }
  bool isWritten = !name->typeArgs.empty();
  if (isWritten && !this->writtenTypeArgsFit(name, generic, bound)) {
    return name;
  }
  if (!isWritten && expected != nullptr) {
    this->bindFromExpected(generic, returnType, *expected, bound);
  }
  bool isFinished =
      this->typeArgInferrer->finishTypeArgs(generic, bound, name->loc(), TypeArgUse::VALUE);
  if (!isFinished || returnType == nullptr) {
    return name;
  }
  this->typeGenericValue(name, generic, returnType, bound);

  return name;
}

bool GenericInstantiator::writtenTypeArgsFit(const NameExpr *name, const FuncSymbol *generic,
                                             std::vector<const Type *> &bound) {
  auto counted = this->typeArgCount(generic, name);
  if (counted.isErr()) {
    this->state.getDiagnostics().error(name->loc(), counted.error().message);

    return false;
  }
  for (size_t index = 0; index < name->typeArgs.size(); ++index) {
    bound[index] = this->typeResolver->resolveTypeOrVoid(name->typeArgs[index]);
  }

  return true;
}

void GenericInstantiator::bindFromExpected(const FuncSymbol *generic, const Type *returnType,
                                           const Signature &expected,
                                           std::vector<const Type *> &bound) {
  llvm::ArrayRef<const Type *> genericTypeParams = generic->typeParams;
  Inference fromParams(genericTypeParams, this->state.getAliasStore(), nullptr);
  Inference fromResult(genericTypeParams, this->state.getAliasStore(), nullptr);
  size_t shared = std::min(generic->params.size(), expected.params.size());
  for (size_t index = 0; index < shared; ++index) {
    if (expected.params[index] != nullptr) {
      fromParams.from(generic->params[index], expected.params[index]);
    }
  }
  if (returnType != nullptr && expected.ret != nullptr && !expected.voidResult) {
    fromResult.from(returnType, expected.ret);
  }
  for (size_t index = 0; index < genericTypeParams.size(); ++index) {
    for (const Inference *inference : {&fromParams, &fromResult}) {
      const auto *type = this->unifiedCandidates(*inference, index);
      if (type != nullptr && bound[index] == nullptr) {
        bound[index] = type;
      }
    }
  }
}

const Type *GenericInstantiator::unifiedCandidates(const Inference &inference, size_t index) const {
  const Type *type = nullptr;
  for (const Type *candidate : inference.getCandidates(index)) {
    type = type != nullptr ? this->typeRelations->unify(type, candidate) : candidate;
    if (type == nullptr) {
      return nullptr;
    }
  }

  return type;
}

void GenericInstantiator::typeGenericValue(NameExpr *name, FuncSymbol *generic,
                                           const Type *returnType,
                                           const std::vector<const Type *> &bound) {
  llvm::ArrayRef<const Type *> genericTypeParams = generic->typeParams;
  auto params = std::vector<const Type *>();
  for (const Type *param : generic->params) {
    const auto *bare = this->typeParams->substituteValue(param, genericTypeParams, bound);
    params.push_back(bare);
  }
  const auto *result = this->typeParams->substitute(returnType, genericTypeParams, bound);
  name->binding = NamedFunction{.func = generic};
  name->type = Type::function(params, generic->minArgs, generic->hasRest, result, generic->hasThis);
  bool isConcrete = this->isConcrete(bound);
  if (!isConcrete || this->state.getFunctionStack().isInsideTrial() || generic->inferring) {
    return;
  }
  auto *instance = this->instantiate(generic, bound, name->loc());
  if (instance != nullptr) {
    name->binding = NamedFunction{.func = instance};
  }
}

bool GenericInstantiator::isConcrete(llvm::ArrayRef<const Type *> bound) const {
  for (const Type *type : bound) {
    if (TypeQueries::mentionsParam(type)) {
      return false;
    }
  }

  return true;
}

bool GenericInstantiator::returnsOwnClass(const FuncSymbol *generic, const Type *returnType) {
  const ClassDecl *classDecl = returnType != nullptr ? this->literalClassIn(returnType) : nullptr;
  auto plain = this->ownClassReturn(generic, classDecl);
  if (plain.isOk()) {
    return false;
  }
  if (!this->state.getGenericStore().isOwnClassReturnReported(generic)) {
    this->state.getGenericStore().markOwnClassReturnReported(generic);
    this->state.getDiagnostics().error(classDecl->literal()->loc(), plain.error().message);
  }

  return true;
}

std::vector<const Type *> GenericInstantiator::exactClassArgs(CallExpr *call,
                                                              const FuncSymbol *generic) {
  auto exact = std::vector<const Type *>();
  if (generic->hasRest) {
    return exact;
  }
  size_t shared = std::min(generic->params.size(), call->args.size());
  for (size_t index = 0; index < shared; ++index) {
    if (this->isGenericClassParam(generic, generic->params[index])) {
      this->takeExactClass(call, index, generic->params.size(), exact);
    }
  }

  return exact;
}

bool GenericInstantiator::isGenericClassParam(const FuncSymbol *generic, const Type *param) const {
  if (param == nullptr || !param->isConstructor()) {
    return false;
  }

  return GenericCheckHelpers::mentionsAnyOf(param, generic->typeParams);
}

std::vector<const CastExpr *> GenericInstantiator::implicitCasts(const Expr *arg) const {
  auto casts = std::vector<const CastExpr *>();
  auto *conversion = dyn_cast<CastExpr>(arg);
  while (conversion != nullptr && conversion->isImplicit()) {
    casts.push_back(conversion);
    conversion = dyn_cast<CastExpr>(conversion->operand);
  }

  return casts;
}

void GenericInstantiator::takeExactClass(CallExpr *call, size_t index, size_t paramCount,
                                         std::vector<const Type *> &exact) {
  Expr *arg = ExprParts::withoutImplicitCasts(call->args[index]);
  const ClassDecl *classDecl = arg->type->classRef();
  if (classDecl == nullptr || classDecl->type == nullptr) {
    return;
  }
  auto dropped = this->implicitCasts(call->args[index]);
  exact.resize(paramCount, nullptr);
  exact[index] = arg->type;
  call->args[index] = arg;
  for (const CastExpr *conversion : dropped) {
    this->state.getReflectStore().forgetConstructorCast(conversion);
  }
}

FuncSymbol *GenericInstantiator::instantiate(FuncSymbol *generic, llvm::ArrayRef<const Type *> args,
                                             llvm::SMLoc loc, llvm::ArrayRef<const Type *> exact) {
  if (this->hasErrorArg(args)) {
    return nullptr;
  }
  auto reps = this->instanceReps(generic, args);
  auto keyTypes = this->instanceKey(reps, exact);
  auto &generics = this->state.getGenericStore();
  auto *known = generics.findFunctionInstance(generic, keyTypes);
  if (known != nullptr) {
    return this->reuseInstance(known, loc, args);
  }
  if (!this->instanceArgsFit(generic, reps, args, loc)) {
    return nullptr;
  }
  InstanceRecord &instance = *generics.addInstanceRecord();
  instance.generic = generic;
  instance.args = reps;
  instance.concrete.assign(args.begin(), args.end());
  instance.exact.assign(exact.begin(), exact.end());
  instance.callLoc = loc;
  instance.requester = this->state.getGenericStore().getCheckingInstance();
  instance.decl = this->instanceDecl(generic, reps, exact);
  generics.rememberFunctionInstance(generic, keyTypes, &instance);
  this->placeInstance(instance);

  return this->checkNewInstance(&instance, loc, args);
}

std::vector<const Type *>
GenericInstantiator::instanceKey(llvm::ArrayRef<const Type *> reps,
                                 llvm::ArrayRef<const Type *> exact) const {
  std::vector<const Type *> key(reps.begin(), reps.end());
  if (!exact.empty()) {
    key.push_back(nullptr);
    key.insert(key.end(), exact.begin(), exact.end());
  }

  return key;
}

bool GenericInstantiator::hasErrorArg(llvm::ArrayRef<const Type *> args) const {
  for (const Type *type : args) {
    if (type->isError()) {
      return true;
    }
  }

  return false;
}

std::vector<const Type *> GenericInstantiator::instanceReps(FuncSymbol *generic,
                                                            llvm::ArrayRef<const Type *> args) {
  auto reps = std::vector<const Type *>();
  for (size_t index = 0; index < args.size(); ++index) {
    bool isExactParam = this->typeParams->isExactTypeParam(generic, index);
    const Type *rep = isExactParam ? args[index] : this->typeParams->representation(args[index]);
    reps.push_back(rep);
  }

  return reps;
}

FuncSymbol *GenericInstantiator::reuseInstance(InstanceRecord *instance, llvm::SMLoc loc,
                                               llvm::ArrayRef<const Type *> args) {
  bool isChecking = this->state.getGenericStore().isCheckingInstance();
  if (isChecking && !instance->checked && !instance->checking) {
    this->walkInstance(instance);
  }
  bool isReportDue = this->state.getGenericStore().isCheckingInstance() || !instance->isReported;
  if (instance->failed && isReportDue) {
    this->reportInstance(instance, loc, args);
  }

  return instance->decl->sym;
}

bool GenericInstantiator::instanceArgsFit(const FuncSymbol *generic,
                                          llvm::ArrayRef<const Type *> reps,
                                          llvm::ArrayRef<const Type *> args, llvm::SMLoc loc) {
  auto bounded = this->chainBounded(generic, args);
  if (bounded.isErr()) {
    this->state.getDiagnostics().error(loc, bounded.error().message);

    return false;
  }
  auto meaningful = this->paramsMeaningful(generic, reps, args);
  if (meaningful.isErr() && !this->state.getGenericStore().isCheckingInstance()) {
    this->state.getDiagnostics().error(loc, meaningful.error().message);
  }

  return meaningful.isOk();
}

void GenericInstantiator::placeInstance(const InstanceRecord &instance) {
  FunctionDecl *original = instance.generic->decl;
  ClassDecl *classDecl = original->cls();
  if (classDecl != nullptr && !original->isStatic()) {
    classDecl->methodInstances.push_back(instance.decl);
    return;
  }
  this->state.getInventoryStore().addInstance(instance.decl);
}

FuncSymbol *GenericInstantiator::checkNewInstance(InstanceRecord *instance, llvm::SMLoc loc,
                                                  llvm::ArrayRef<const Type *> args) {
  bool isTemplate = this->lateClasses->isTemplate(instance->generic);
  if (!isTemplate && !this->state.getGenericStore().isCheckingInstance()) {
    this->state.getGenericStore().addPendingInstance(instance);

    return instance->decl->sym;
  }
  if (isTemplate) {
    this->lateClasses->walkTemplateInstance(instance);
  } else {
    this->walkInstance(instance);
  }
  if (instance->failed) {
    this->reportInstance(instance, loc, args);
  }

  return instance->decl->sym;
}

FunctionDecl *GenericInstantiator::instanceDecl(FuncSymbol *generic,
                                                llvm::ArrayRef<const Type *> args,
                                                llvm::ArrayRef<const Type *> exact) {
  FunctionDecl *original = generic->decl;
  auto *copy = this->returnInferrer->freshCopy(original);
  copy->isInstance = true;
  copy->typeArgs.assign(args.begin(), args.end());
  auto *function = this->instanceSymbol(generic, copy, args);
  this->substituteCopyParams(original, copy, generic->typeParams, args);
  this->applyExactParams(function, copy, exact);
  copy->sym = function;
  this->argListChecker->expandArgumentList(copy, function);
  auto *home = this->state.getModuleStates().findDeclModule(original);
  if (home != nullptr) {
    this->state.getModuleStates().rememberDeclModule(copy, home);
  }

  return copy;
}

FuncSymbol *GenericInstantiator::instanceSymbol(const FuncSymbol *generic, FunctionDecl *copy,
                                                llvm::ArrayRef<const Type *> args) {
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  auto *function = this->state.getAst().newFunc();
  function->setCompleter(this->state.getCompleter());
  function->name = generic->name;
  function->loc = generic->loc;
  function->decl = copy;
  for (const Type *param : generic->params) {
    const auto *instanceParam = this->typeParams->substituteValue(param, params, args);
    function->params.push_back(instanceParam);
  }
  function->minArgs = generic->minArgs;
  function->hasRest = generic->hasRest;
  const Type *returned =
      generic->hasReturnType()
          ? this->typeParams->substitute(generic->getKnownReturnType(), params, args)
          : nullptr;
  function->setReturnType(returned);
  if (generic->predicate.has_value()) {
    const auto *type = this->typeParams->substitute(generic->predicate->type, params, args);
    function->predicate = TypePredicate{.param = generic->predicate->param, .type = type};
  }

  return function;
}

void GenericInstantiator::substituteCopyParams(const FunctionDecl *original, FunctionDecl *copy,
                                               llvm::ArrayRef<const Type *> params,
                                               llvm::ArrayRef<const Type *> args) {
  size_t shared = std::min(copy->params.size(), original->params.size());
  for (size_t index = 0; index < shared; ++index) {
    const Type *declared = original->params[index].declaredType;
    copy->params[index].declaredType = this->typeParams->substituteValue(declared, params, args);
  }
}

void GenericInstantiator::applyExactParams(FuncSymbol *function, FunctionDecl *copy,
                                           llvm::ArrayRef<const Type *> exact) const {
  size_t shared = std::min({exact.size(), function->params.size(), copy->params.size()});
  for (size_t index = 0; index < shared; ++index) {
    if (exact[index] != nullptr) {
      copy->params[index].declaredType = exact[index];
      function->params[index] = exact[index];
    }
  }
}

std::vector<Diagnostic> GenericInstantiator::walkCopy(InstanceRecord *instance,
                                                      FunctionDecl *copy) {
  auto &diagnostics = this->state.getDiagnostics();
  auto saved = this->returnInferrer->enterBody(copy);
  this->state.getGenericStore().rememberCheckingInstance(instance);
  diagnostics.pushList(DiagnosticRoute::OWNER);
  this->functionChecker->walkFunctionFromScratch(copy);
  auto copyDiagnostics = diagnostics.popList();
  this->returnInferrer->leaveBody(saved);

  return copyDiagnostics;
}

void GenericInstantiator::walkInstance(InstanceRecord *instance) {
  instance->checking = true;
  auto &diagnostics = this->state.getDiagnostics();
  auto instanceDiagnostics = std::vector<Diagnostic>();
  if (!this->state.getGenericStore().isBrokenGeneric(instance->generic)) {
    instanceDiagnostics = this->walkCopy(instance, instance->decl);
  }
  instance->checking = false;
  instance->checked = true;
  const auto *firstError = diagnostics.findFirstError(instanceDiagnostics);
  if (firstError == nullptr) {
    return;
  }
  instance->failed = true;
  instance->message = firstError->message;
  instance->errorLoc = firstError->loc;
  if (instance->concrete != instance->args) {
    this->findPreciseError(instance);
  }
}

void GenericInstantiator::findPreciseError(InstanceRecord *instance) {
  auto *copy = this->instanceDecl(instance->generic, instance->concrete, instance->exact);
  auto preciseDiagnostics = this->walkCopy(instance, copy);
  const auto *preciseError = this->state.getDiagnostics().findFirstError(preciseDiagnostics);
  if (preciseError != nullptr) {
    instance->message = preciseError->message;
    instance->errorLoc = preciseError->loc;
  }
}

void GenericInstantiator::reportInstance(InstanceRecord *instance, llvm::SMLoc callLoc,
                                         llvm::ArrayRef<const Type *> concrete) {
  auto destination = this->state.getDiagnostics().getCurrentDestination();
  instance->isReported = destination != DiagnosticRoute::DISCARD;
  if (destination == DiagnosticRoute::OWNER) {
    this->state.getGenericStore().rememberFailedCall(callLoc, instance);
    this->state.getDiagnostics().error(callLoc, instance->message);
    return;
  }
  const FuncSymbol *generic = instance->generic;
  auto bindings = GenericCheckHelpers::bindingNames(generic->typeParams, concrete);
  auto message = ErrorTextInstanceCall::cannotCall(generic->name, bindings, instance->message);
  this->state.getDiagnostics().error(callLoc, message);
  this->noteFailedChain(instance);
}

void GenericInstantiator::noteFailedChain(const InstanceRecord *start) {
  auto shown = llvm::SmallPtrSet<const char *, 8>();
  const InstanceRecord *at = start;
  while (at != nullptr && at->errorLoc.isValid()) {
    bool isFirstShown = shown.insert(at->errorLoc.getPointer()).second;
    if (!isFirstShown) {
      return;
    }
    const auto *next = this->state.getGenericStore().findFailedCall(at->errorLoc);
    auto where = this->failedCallNote(at, next);
    this->state.getDiagnostics().note(at->errorLoc, where);
    at = next;
  }
}

std::string GenericInstantiator::failedCallNote(const InstanceRecord *at,
                                                const InstanceRecord *next) const {
  llvm::StringRef name = GenericCheckHelpers::recordName(at);
  if (next == nullptr) {
    return ErrorTextInstanceCall::here(name);
  }
  bool isClass = next->cls != nullptr;

  return ErrorTextInstanceCall::inWhich(name, isClass, GenericCheckHelpers::recordName(next));
}

void GenericInstantiator::walkInstances() {
  auto &generics = this->state.getGenericStore();
  for (int index = 0; index < generics.countPendingInstances(); ++index) {
    auto *instance = generics.getPendingInstance(index);
    if (!instance->checked) {
      this->walkPendingInstance(instance);
    }
  }
  generics.forgetPendingInstances();
}

void GenericInstantiator::walkPendingInstance(InstanceRecord *instance) {
  if (instance->cls != nullptr) {
    this->genericClassChecker->walkClassInstance(instance);
    if (instance->failed) {
      this->genericClassChecker->reportClassInstance(instance, instance->callLoc);
    }
    return;
  }
  this->walkInstance(instance);
  if (instance->failed) {
    this->reportInstance(instance, instance->callLoc, instance->concrete);
  }
}

}
