#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/program/ast/ast_cloner/ast_cloner.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/stream/stream_kind_table/stream_kind_table.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/semantic/generics/inference/inference.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/library/events/event_maps/event_maps.h"
#include "compiler/semantic/library/stream/chunk_types/chunk_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;

namespace tz {

namespace {

constexpr int kMaxClassChain = 16;
}

GenericClassInstantiator::GenericClassInstantiator(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->eventMaps = &all.eventMaps;
  this->chunkTypes = &all.chunkTypes;
  this->classBases = &all.classBases;
  this->classLinks = &all.classLinks;
  this->typeParams = &all.typeParams;
  this->classDeclarer = &all.classDeclarer;
  this->genericClassChecker = &all.genericClassChecker;
}

GenericClassInstantiator::ResultClassMade
GenericClassInstantiator::instantiateClass(ClassDecl *generic, std::vector<const Type *> args,
                                           llvm::SMLoc use) {
  this->genericClassChecker->declareClassTypeParams(generic);
  llvm::SMLoc loc = use.isValid() ? use : generic->loc();
  auto counted = this->checkArgumentCount(generic, args);
  if (counted.isErr()) {
    return err(counted.error());
  }
  auto filled = this->argumentsWithDefaults(generic, std::move(args));
  if (filled.isErr()) {
    return err(filled.error());
  }
  const std::vector<const Type *> &allArgs = filled.value();
  auto fitting = this->checkArguments(generic, allArgs);
  if (fitting.isErr()) {
    return err(fitting.error());
  }
  auto mapped = this->checkEventMap(generic, allArgs, loc);
  if (mapped.isErr()) {
    return err(mapped.error());
  }
  ClassDecl *known = this->state.getGenericStore().findClassInstance(generic, allArgs);
  if (known != nullptr) {
    this->reuseInstance(known, use, loc);

    return ok(known);
  }
  auto bounded = this->checkChainBounded(generic);
  if (bounded.isErr()) {
    return err(bounded.error());
  }
  auto *instance = this->makeInstance(generic, allArgs);
  this->rememberCheck(instance, use);
  this->fillOrPostpone(instance);
  this->walkOrPostpone(instance, loc);

  return ok(instance);
}

GenericClassInstantiator::ResultCanBeMade
GenericClassInstantiator::checkArgumentCount(const ClassDecl *generic,
                                             llvm::ArrayRef<const Type *> args) const {
  size_t count = generic->typeArgs.size();
  if (args.size() > count) {
    auto message = ErrorTextClassInstance::tooMany(generic->name, count, args.size());

    return err(GenericClassInstantiatorInstantiateClassError{
        .code = GenericClassInstantiatorInstantiateClassErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok();
}

GenericClassInstantiator::ResultArgsKnown
GenericClassInstantiator::argumentsWithDefaults(const ClassDecl *generic,
                                                std::vector<const Type *> args) {
  const std::vector<const Type *> &params = generic->typeArgs;
  auto defaults = this->state.getGenericStore().getClassDefaults(generic);
  for (size_t index = args.size(); index < params.size(); ++index) {
    const Type *defaultType = index < defaults.size() ? defaults[index] : nullptr;
    auto present = this->checkDefaultPresent(generic, defaultType);
    if (present.isErr()) {
      return err(present.error());
    }
    std::vector<const Type *> known = args;
    known.resize(params.size(), nullptr);
    const auto *filled = this->typeParams->substitute(defaultType, params, known);
    args.push_back(filled);
  }

  return ok(std::move(args));
}

GenericClassInstantiator::ResultCanBeMade
GenericClassInstantiator::checkDefaultPresent(const ClassDecl *generic,
                                              const Type *defaultType) const {
  if (this->isEmitterClass(generic)) {
    auto message = ErrorTextClassInstance::eventMap();

    return err(GenericClassInstantiatorInstantiateClassError{
        .code = GenericClassInstantiatorInstantiateClassErrorCode::EVENT_MAP_MISSING,
        .message = message,
    });
  }
  if (defaultType == nullptr && this->isStreamClass(generic)) {
    auto message = ErrorTextClassInstance::chunks(generic->name);

    return err(GenericClassInstantiatorInstantiateClassError{
        .code = GenericClassInstantiatorInstantiateClassErrorCode::CHUNK_TYPE_MISSING,
        .message = message,
    });
  }
  if (defaultType == nullptr) {
    auto message = ErrorTextClassInstance::needs(generic->name, generic->typeArgs.size());

    return err(GenericClassInstantiatorInstantiateClassError{
        .code = GenericClassInstantiatorInstantiateClassErrorCode::TYPE_ARGUMENTS_MISSING,
        .message = message,
    });
  }

  return ok();
}

bool GenericClassInstantiator::isEmitterClass(const ClassDecl *classDecl) const {
  return classDecl->library() &&
         LibraryClassTable::isName(classDecl->name, LibraryClass::EVENT_EMITTER);
}

bool GenericClassInstantiator::isStreamClass(const ClassDecl *classDecl) const {
  auto streamKind = StreamKindTable::find(classDecl->name);

  return classDecl->library() && streamKind.has_value();
}

GenericClassInstantiator::ResultCanBeMade
GenericClassInstantiator::checkArguments(const ClassDecl *generic,
                                         const std::vector<const Type *> &args) {
  for (size_t index = 0; index < generic->typeArgs.size(); ++index) {
    auto fitting = this->checkArgument(generic, args, index);
    if (fitting.isErr()) {
      return err(fitting.error());
    }
  }

  return ok();
}

GenericClassInstantiator::ResultCanBeMade
GenericClassInstantiator::checkArgument(const ClassDecl *generic,
                                        const std::vector<const Type *> &args, size_t index) {
  const std::vector<const Type *> &params = generic->typeArgs;
  const Type *arg = args[index];
  const Type *param = params[index];
  if (arg->isError()) {
    return err(GenericClassInstantiatorInstantiateClassError{
        .code = GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (arg->isUndefined()) {
    auto message = ErrorTextClassInstance::undefinedArg(arg->name(), generic->name);

    return err(GenericClassInstantiatorInstantiateClassError{
        .code = GenericClassInstantiatorInstantiateClassErrorCode::UNDEFINED_TYPE_ARGUMENT,
        .message = message,
    });
  }
  if (!this->typeParams->isSatisfying(arg, param, params, args)) {
    auto constraint = this->constraintText(generic, index);
    auto message =
        ErrorTextClassInstance::unsatisfied(arg->name(), constraint, param->name(), generic->name);

    return err(GenericClassInstantiatorInstantiateClassError{
        .code = GenericClassInstantiatorInstantiateClassErrorCode::CONSTRAINT_UNSATISFIED,
        .message = message,
    });
  }

  return ok();
}

std::string GenericClassInstantiator::constraintText(const ClassDecl *generic, size_t index) const {
  const TypeParamDecl &typeParam = generic->typeParams[index];
  bool isNamed =
      typeParam.constraint.has_value() && typeParam.constraint->kind() == TypeRefKind::NAME_REF;
  if (isNamed) {
    return typeParam.constraint->name.str();
  }
  const Type *param = generic->typeArgs[index];
  auto allowed = std::vector<const Type *>();
  if (param->constraint() != nullptr) {
    allowed.push_back(param->constraint());
  }
  llvm::append_range(allowed, param->alternatives());

  return GenericCheckHelpers::typeNames(allowed, " | ");
}

GenericClassInstantiator::ResultCanBeMade
GenericClassInstantiator::checkEventMap(const ClassDecl *generic, llvm::ArrayRef<const Type *> args,
                                        llvm::SMLoc loc) {
  if (!this->isEmitterClass(generic)) {
    return ok();
  }
  auto fits = this->eventMaps->eventMapFits(args[0]);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(loc, fits.error().message);

    return err(GenericClassInstantiatorInstantiateClassError{
        .code = GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }

  return ok();
}

void GenericClassInstantiator::reuseInstance(const ClassDecl *known, llvm::SMLoc use,
                                             llvm::SMLoc loc) {
  auto *check = this->state.getGenericStore().findClassCheck(known);
  if (check == nullptr) {
    return;
  }
  if (!check->callLoc.isValid()) {
    check->callLoc = use;
  }
  bool isReportDue = check->checked && check->failed && !check->isReported;
  if (isReportDue) {
    this->genericClassChecker->reportClassInstance(check, loc);
  }
}

GenericClassInstantiator::ResultCanBeMade
GenericClassInstantiator::checkChainBounded(const ClassDecl *generic) {
  auto &generics = this->state.getGenericStore();
  if (generics.countFillingOf(generic) < kMaxClassChain) {
    return ok();
  }
  bool isReported = generics.isEndlessReported(generic);
  generics.markEndlessReported(generic);
  if (isReported) {
    return err(GenericClassInstantiatorInstantiateClassError{
        .code = GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  auto message = ErrorTextClassInstance::endless(generic->name);

  return err(GenericClassInstantiatorInstantiateClassError{
      .code = GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES,
      .message = message,
  });
}

ClassDecl *GenericClassInstantiator::makeInstance(ClassDecl *generic,
                                                  const std::vector<const Type *> &args) {
  AstCloner cloner(this->state.getAst());
  ClassDecl *instance = cloner.copyClass(generic);
  instance->decorators.clear();
  instance->syntax = StatementClass{};
  instance->generics = GenericInstance{.generic = generic};
  instance->typeArgs = args;
  instance->isAbstract = generic->isAbstract;
  instance->origin = generic->origin;
  instance->checkOnly = this->isMentioningParam(args);
  instance->stage = ClassStage::RESOLVED;
  instance->declared = true;
  instance->id = this->state.getClassStore().takeClassId();
  auto display = this->instanceName(generic, args);
  auto *type = Type::classType(display, instance);
  type->setTypeArgs(args);
  instance->type = type;
  this->registerInstance(generic, instance);

  return instance;
}

bool GenericClassInstantiator::isMentioningParam(llvm::ArrayRef<const Type *> args) const {
  for (const Type *arg : args) {
    if (TypeQueries::mentionsParam(arg)) {
      return true;
    }
  }

  return false;
}

std::string GenericClassInstantiator::instanceName(const ClassDecl *generic,
                                                   llvm::ArrayRef<const Type *> args) const {
  auto defaults = this->state.getGenericStore().getClassDefaults(generic);
  size_t shown = GenericCheckHelpers::shownArgumentCount(defaults, args);
  if (shown == 0) {
    return generic->name.str();
  }
  auto names = GenericCheckHelpers::typeNames(args.take_front(shown), ", ");

  return generic->name.str() + "<" + names + ">";
}

void GenericClassInstantiator::registerInstance(ClassDecl *generic, ClassDecl *instance) {
  this->state.getClassStore().addClass(instance);
  generic->generics.as<GenericTemplate>()->instances.push_back(instance);
  this->state.getGenericStore().rememberClassInstance(generic, instance->typeArgs, instance);
  auto *home = this->state.getModuleStates().findDeclModule(generic);
  if (home != nullptr) {
    this->state.getModuleStates().rememberDeclModule(instance, home);
  }
}

void GenericClassInstantiator::rememberCheck(ClassDecl *instance, llvm::SMLoc use) {
  if (instance->checkOnly) {
    return;
  }
  auto &generics = this->state.getGenericStore();
  auto *check = generics.addInstanceRecord();
  check->cls = instance;
  check->callLoc = use;
  check->requester = generics.getCheckingInstance();
  generics.rememberClassCheck(instance, check);
}

void GenericClassInstantiator::fillOrPostpone(ClassDecl *instance) {
  auto &generics = this->state.getGenericStore();
  if (!this->state.getClassStore().hasDeclaredMethods()) {
    generics.addUnfilledClass(instance);
    return;
  }
  generics.enterFilling(instance);
  this->fillInstance(instance);
  generics.leaveFilling();
  if (this->state.getClassStore().hasLinkedClasses()) {
    this->linkInstance(instance);
  }
}

void GenericClassInstantiator::walkOrPostpone(const ClassDecl *instance, llvm::SMLoc loc) {
  auto &generics = this->state.getGenericStore();
  auto *check = generics.findClassCheck(instance);
  if (check == nullptr) {
    return;
  }
  if (!generics.isCheckingInstance()) {
    generics.addPendingInstance(check);
    return;
  }
  this->genericClassChecker->walkClassInstance(check);
  if (check->failed) {
    this->genericClassChecker->reportClassInstance(check, loc);
  }
}

void GenericClassInstantiator::declareInstanceMember(ClassDecl *instance,
                                                     FunctionDecl *functionDecl,
                                                     FunctionDecl *generic) {
  FuncSymbol *genericFunction = generic->sym;
  auto *function = this->makeInstanceSymbol(functionDecl, generic);
  if (genericFunction == nullptr) {
    return;
  }
  auto instanceParams = this->iteratedParams(instance, genericFunction->typeParams);
  auto types = MemberTypes{
      .instance = instance,
      .genericParams = genericFunction->typeParams,
      .instanceParams = instanceParams,
  };
  functionDecl->typeArgs = instanceParams;
  function->typeParams = instanceParams;
  this->fillInstanceSignature(types, functionDecl, generic, function);
  auto *home = this->state.getModuleStates().findDeclModule(generic);
  if (home != nullptr) {
    this->state.getModuleStates().rememberDeclModule(functionDecl, home);
  }
}

FuncSymbol *GenericClassInstantiator::makeInstanceSymbol(FunctionDecl *functionDecl,
                                                         FunctionDecl *generic) {
  auto *function = this->state.getAst().newFunc();
  function->setCompleter(this->state.getCompleter());
  function->name = functionDecl->name;
  function->loc = functionDecl->loc();
  function->decl = functionDecl;
  functionDecl->sym = function;
  functionDecl->generic = generic;
  ClassMember *member = functionDecl->member();
  if (member != nullptr) {
    member->variant = generic->variant();
  }
  Overriding *overriding = functionDecl->overriding();
  if (overriding != nullptr) {
    overriding->overridden = generic->overridden();
    overriding->openToLate = generic->openToLate();
  }

  return function;
}

std::vector<const Type *>
GenericClassInstantiator::iteratedParams(ClassDecl *instance, llvm::ArrayRef<const Type *> params) {
  auto instanceParams = std::vector<const Type *>();
  for (const Type *param : params) {
    const auto *instanceParam = this->iteratedParam(instance, param);
    instanceParams.push_back(instanceParam);
  }

  return instanceParams;
}

const Type *GenericClassInstantiator::iteratedParam(ClassDecl *instance, const Type *param) {
  const Type *iterated = param->iterated();
  if (iterated == nullptr) {
    return param;
  }
  if (!GenericCheckHelpers::mentionsAnyOf(iterated, instance->generic()->typeArgs)) {
    return param;
  }
  const auto *instanceIterated = this->genericClassChecker->instanceType(instance, iterated);
  llvm::StringRef iterable = TypeNameTable::getName(TypeName::ITERABLE);
  auto *instanceParam = Type::param(iterable.str() + "<" + instanceIterated->name().str() + ">");
  instanceParam->setIterated(instanceIterated);

  return instanceParam;
}

const Type *GenericClassInstantiator::substituted(const MemberTypes &types, const Type *type) {
  if (type == nullptr) {
    return nullptr;
  }
  const auto *instanceType = this->genericClassChecker->instanceType(types.instance, type);

  return this->typeParams->substitute(instanceType, types.genericParams, types.instanceParams);
}

const Type *GenericClassInstantiator::substitutedValue(const MemberTypes &types, const Type *type) {
  if (type == nullptr) {
    return nullptr;
  }
  const auto *instanceType = this->genericClassChecker->instanceValueType(types.instance, type);

  return this->typeParams->substituteValue(instanceType, types.genericParams, types.instanceParams);
}

void GenericClassInstantiator::fillInstanceSignature(const MemberTypes &types,
                                                     FunctionDecl *functionDecl,
                                                     FunctionDecl *generic, FuncSymbol *function) {
  FuncSymbol *genericFunction = generic->sym;
  for (const Type *defaultType : genericFunction->typeDefaults) {
    const auto *instanceDefault = this->substituted(types, defaultType);
    function->typeDefaults.push_back(instanceDefault);
  }
  for (const Type *param : genericFunction->params) {
    const auto *instanceParam = this->substitutedValue(types, param);
    function->params.push_back(instanceParam);
  }
  this->fillInstanceParams(types, functionDecl, generic);
  function->minArgs = genericFunction->minArgs;
  function->hasRest = genericFunction->hasRest;
  const Type *genericReturn = genericFunction->getKnownReturnType();
  const auto *instanceReturn = this->substituted(types, genericReturn);
  function->setReturnType(instanceReturn);
  function->discardsResult = genericFunction->discardsResult;
  if (genericFunction->predicate.has_value()) {
    const auto *type = this->substituted(types, genericFunction->predicate->type);
    function->predicate = TypePredicate{.param = genericFunction->predicate->param, .type = type};
  }
}

void GenericClassInstantiator::fillInstanceParams(const MemberTypes &types,
                                                  FunctionDecl *functionDecl,
                                                  const FunctionDecl *generic) {
  for (size_t index = functionDecl->params.size(); index < generic->params.size(); ++index) {
    functionDecl->params.push_back(generic->params[index]);
  }
  for (size_t index = 0; index < generic->params.size(); ++index) {
    const Type *genericType = generic->params[index].declaredType;
    functionDecl->params[index].declaredType = this->substitutedValue(types, genericType);
  }
  for (size_t index = functionDecl->typeParams.size(); index < generic->typeParams.size();
       ++index) {
    functionDecl->typeParams.push_back(generic->typeParams[index]);
  }
}

void GenericClassInstantiator::fillInstance(ClassDecl *instance) {
  this->linkInstanceBases(instance);
  this->chunkTypes->addStreamBases(instance, instance->loc());
  this->fillInstanceFields(instance);
  this->declareInstanceMembers(instance);
  if (instance->generic()->init != nullptr) {
    this->makeInstanceInit(instance);
  }
  this->walkInstanceVariants(instance);
}

void GenericClassInstantiator::addInstanceBase(ClassDecl *instance, const Type *base) {
  this->classBases->addBase(instance->type, base);
  if (instance->isEmitted()) {
    this->state.getInventoryStore().addSubtype(instance->type);
  }
}

void GenericClassInstantiator::linkInstanceBases(ClassDecl *instance) {
  this->linkInstanceParent(instance);
  for (const Type *interfaceType : instance->generic()->implemented) {
    const auto *type = this->genericClassChecker->instanceType(instance, interfaceType);
    if (!type->isError() && !llvm::is_contained(instance->implemented, type)) {
      instance->implemented.push_back(type);
      this->addInstanceBase(instance, type);
    }
  }
}

void GenericClassInstantiator::linkInstanceParent(ClassDecl *instance) {
  const ClassDecl *genericParent = instance->generic()->parent;
  if (genericParent == nullptr) {
    return;
  }
  const auto *parentType = this->genericClassChecker->instanceType(instance, genericParent->type);
  auto *parent = this->classDeclarer->classOf(parentType);
  if (parent != nullptr) {
    instance->parent = parent;
    parent->children.push_back(instance);
    this->addInstanceBase(instance, parent->type);
  }
}

void GenericClassInstantiator::fillInstanceFields(ClassDecl *instance) {
  ClassDecl *generic = instance->generic();
  for (size_t index = 0; index < instance->fields.size() && index < generic->fields.size();
       ++index) {
    this->fillInstanceField(instance, instance->fields[index], generic->fields[index]);
  }
}

void GenericClassInstantiator::fillInstanceField(ClassDecl *instance, ClassField &field,
                                                 const ClassField &genericField) {
  field.presence = genericField.presence;
  field.redeclared = genericField.redeclared;
  if (field.isStatic()) {
    return;
  }
  const FunctionDecl *genericMethod = genericField.method();
  if (genericMethod == nullptr) {
    field.resolved = this->genericClassChecker->instanceValueType(instance, genericField.resolved);
    return;
  }
  field.resolved = this->genericClassChecker->instanceType(instance, genericField.resolved);
  FunctionDecl *functionDecl = cast<FunctionExpr>(field.init)->fn;
  functionDecl->name = genericMethod->name;
  functionDecl->role = ClassMember{
      .cls = instance,
      .access = genericMethod->access(),
      .isOverride = genericMethod->isOverride(),
  };
  functionDecl->place = FunctionPlace::DECLARED;
  field.form = ArrowMethodField{.method = functionDecl};
}

void GenericClassInstantiator::declareInstanceMembers(ClassDecl *instance) {
  ClassDecl *generic = instance->generic();
  for (size_t index = 0; index < instance->methods.size() && index < generic->methods.size();
       ++index) {
    if (!generic->methods[index]->isStatic()) {
      this->declareInstanceMember(instance, instance->methods[index], generic->methods[index]);
    }
  }
  for (size_t index = 0;
       index < instance->constructors.size() && index < generic->constructors.size(); ++index) {
    this->declareInstanceMember(instance, instance->constructors[index],
                                generic->constructors[index]);
  }
  for (size_t index = 0; index < instance->fields.size() && index < generic->fields.size();
       ++index) {
    FunctionDecl *method = instance->fields[index].method();
    FunctionDecl *genericMethod = generic->fields[index].method();
    if (method != nullptr && genericMethod != nullptr) {
      this->declareInstanceMember(instance, method, genericMethod);
    }
  }
}

void GenericClassInstantiator::makeInstanceInit(ClassDecl *instance) {
  auto sets = this->classDeclarer->fieldSets(instance);
  auto *init = this->classDeclarer->makeInitDecl(instance, std::move(sets));
  this->declareInstanceMember(instance, init, instance->generic()->init);
  init->sym->setReturnType(Type::voidTy());
  instance->init = init;
}

void GenericClassInstantiator::walkInstanceVariants(ClassDecl *instance) {
  auto &diagnostics = this->state.getDiagnostics();
  diagnostics.pushList(DiagnosticRoute::OWNER);
  this->classDeclarer->walkVariants(instance);
  auto variantDiagnostics = diagnostics.popList();
  const auto *firstError = diagnostics.findFirstError(variantDiagnostics);
  if (firstError == nullptr) {
    return;
  }
  auto *check = this->state.getGenericStore().findClassCheck(instance);
  if (check != nullptr && !check->failed) {
    check->failed = true;
    check->message = firstError->message;
    check->errorLoc = firstError->loc;
  }
}

void GenericClassInstantiator::linkInstance(ClassDecl *instance) {
  this->state.getDiagnostics().pushList(DiagnosticRoute::DISCARD);
  this->classLinks->linkOverrides(instance);
  this->state.getDiagnostics().popList();
  this->classLinks->buildVtable(instance);
}

void GenericClassInstantiator::markOverridden(FunctionDecl *functionDecl) {
  this->setOverridden(functionDecl);
  FunctionDecl *generic = functionDecl->generic != nullptr ? functionDecl->generic : functionDecl;
  ClassDecl *classDecl = generic->cls();
  if (classDecl == nullptr || !classDecl->isGeneric()) {
    return;
  }
  this->setOverridden(generic);
  for (ClassDecl *instance : classDecl->instances()) {
    this->markInstanceCopies(instance, generic);
  }
}

void GenericClassInstantiator::setOverridden(FunctionDecl *functionDecl) const {
  Overriding *overriding = functionDecl->overriding();
  if (overriding != nullptr) {
    overriding->overridden = true;
  }
}

void GenericClassInstantiator::markInstanceCopies(const ClassDecl *instance,
                                                  const FunctionDecl *generic) const {
  for (FunctionDecl *method : this->classLinks->ownMethods(instance)) {
    if (method->generic == generic) {
      this->setOverridden(method);
    }
  }
}

}
