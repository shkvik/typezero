#include "compiler/semantic/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/classes/class_member_checker/class_member_checker.h"
#include "compiler/semantic/bodies/classes/method_call_checker/method_call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/module_declarer/module_declarer.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/semantic/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/stream/chunk_types/chunk_types.h"
#include "compiler/semantic/library/stream/stream_new_checker/stream_new_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

GenericClassChecker::GenericClassChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->chunkTypes = &all.chunkTypes;
  this->typeParams = &all.typeParams;
  this->classLinks = &all.classLinks;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->typeResolver = &all.typeResolver;
  this->classDeclarer = &all.classDeclarer;
  this->emitterChecker = &all.emitterChecker;
  this->moduleDeclarer = &all.moduleDeclarer;
  this->returnInferrer = &all.returnInferrer;
  this->functionChecker = &all.functionChecker;
  this->typeArgInferrer = &all.typeArgInferrer;
  this->streamNewChecker = &all.streamNewChecker;
  this->methodCallChecker = &all.methodCallChecker;
  this->classMemberChecker = &all.classMemberChecker;
  this->genericClassInstantiator = &all.genericClassInstantiator;
}

const ClassDecl *GenericClassChecker::genericInstanceOf(const Type *type,
                                                        const ClassDecl *generic) {
  const ClassDecl *classDecl = type != nullptr ? type->classDecl() : nullptr;
  while (classDecl != nullptr) {
    if (classDecl == generic || classDecl->generic() == generic) {
      return classDecl;
    }
    classDecl = classDecl->parent;
  }

  return nullptr;
}

void GenericClassChecker::declareClassTypeParams(ClassDecl *classDecl) {
  if (!classDecl->isGeneric() || !classDecl->typeArgs.empty()) {
    return;
  }
  auto topTypes = this->state.getScopeStack().getTopTypes();
  this->state.getScopeStack().enterOnlyTypes(topTypes);
  auto *here = this->state.getModuleStates().getCurrentModule();
  auto *home = this->state.getModuleStates().findDeclModule(classDecl);
  bool isMoved = home != nullptr && home != here && this->state.getScopeStack().isTopLevel();
  if (isMoved) {
    this->moduleDeclarer->useModule(home);
  }
  this->state.getScopeStack().openTypeScope();
  auto defaults = std::vector<const Type *>();
  classDecl->typeArgs = this->typeParams->declareTypeParams(classDecl->typeParams, &defaults);
  this->state.getScopeStack().closeTypeScope();
  if (isMoved) {
    this->moduleDeclarer->useModule(here);
  }
  this->state.getScopeStack().leaveOnlyTypes();
  this->state.getGenericStore().rememberClassDefaults(classDecl, std::move(defaults));
  auto *type = const_cast<Type *>(classDecl->type);
  type->setTypeArgs(classDecl->typeArgs);
  auto params = GenericCheckHelpers::typeNames(classDecl->typeArgs, ", ");
  type->rename(classDecl->name.str() + "<" + params + ">");
  this->state.getGenericStore().rememberClassInstance(classDecl, classDecl->typeArgs, classDecl);
}

void GenericClassChecker::bindClassTypeParams(const ClassDecl *classDecl) {
  if (classDecl == nullptr || classDecl->typeParams.empty()) {
    return;
  }
  if (classDecl->typeParams.size() == classDecl->typeArgs.size()) {
    this->typeParams->bindTypeParams(classDecl->typeParams, classDecl->typeArgs);
  }
}

const Type *GenericClassChecker::instanceType(const ClassDecl *instance, const Type *type) {
  if (type == nullptr || instance->generic() == nullptr) {
    return type;
  }

  return this->typeParams->substitute(type, instance->generic()->typeArgs, instance->typeArgs);
}

const Type *GenericClassChecker::instanceValueType(const ClassDecl *instance, const Type *type) {
  if (type == nullptr || instance->generic() == nullptr) {
    return type;
  }

  return this->typeParams->substituteValue(type, instance->generic()->typeArgs, instance->typeArgs);
}

void GenericClassChecker::walkClassInstance(InstanceRecord *instance) {
  if (instance->checked) {
    return;
  }
  instance->checking = true;
  auto instanceDiagnostics = std::vector<Diagnostic>();
  ClassDecl *classDecl = instance->cls;
  if (!this->state.getClassStore().isBrokenClass(classDecl->generic())) {
    this->classDeclarer->completeClass(classDecl);
    instanceDiagnostics = this->walkInstanceBodies(instance);
  }
  instance->checking = false;
  instance->checked = true;
  const auto *firstError = this->state.getDiagnostics().findFirstError(instanceDiagnostics);
  if (firstError == nullptr || instance->failed) {
    return;
  }
  instance->failed = true;
  instance->message = firstError->message;
  instance->errorLoc = firstError->loc;
}

std::vector<FunctionDecl *> GenericClassChecker::instanceBodies(const ClassDecl *classDecl) const {
  auto bodies = std::vector<FunctionDecl *>();
  if (classDecl->init != nullptr) {
    bodies.push_back(classDecl->init);
  }
  bodies.insert(bodies.end(), classDecl->constructors.begin(), classDecl->constructors.end());
  for (FunctionDecl *method : this->classLinks->ownMethods(classDecl)) {
    if (!method->isAbstract() && method->typeParams.empty()) {
      bodies.push_back(method);
    }
  }

  return bodies;
}

std::vector<Diagnostic> GenericClassChecker::walkInstanceBodies(InstanceRecord *instance) {
  auto instanceDiagnostics = std::vector<Diagnostic>();
  for (FunctionDecl *functionDecl : this->instanceBodies(instance->cls)) {
    if (!functionDecl->checked && functionDecl->sym != nullptr) {
      auto bodyDiagnostics = this->walkInstanceBody(instance, functionDecl);
      llvm::append_range(instanceDiagnostics, bodyDiagnostics);
    }
  }

  return instanceDiagnostics;
}

std::vector<Diagnostic> GenericClassChecker::walkInstanceBody(InstanceRecord *instance,
                                                              FunctionDecl *functionDecl) {
  auto &diagnostics = this->state.getDiagnostics();
  if (!functionDecl->sym->hasReturnType()) {
    this->returnInferrer->inferReturnType(functionDecl->sym);
  }
  auto saved = this->returnInferrer->enterBody(functionDecl);
  this->state.getGenericStore().rememberCheckingInstance(instance);
  diagnostics.pushList(DiagnosticRoute::OWNER);
  this->functionChecker->walkFunctionFromScratch(functionDecl);
  auto bodyDiagnostics = diagnostics.popList();
  this->returnInferrer->leaveBody(saved);

  return bodyDiagnostics;
}

void GenericClassChecker::reportClassInstance(InstanceRecord *instance, llvm::SMLoc use) {
  llvm::SMLoc loc = use.isValid() ? use : instance->cls->generic()->loc();
  auto destination = this->state.getDiagnostics().getCurrentDestination();
  instance->isReported = destination != DiagnosticRoute::DISCARD;
  if (destination == DiagnosticRoute::OWNER) {
    this->state.getGenericStore().rememberFailedCall(loc, instance);
    this->state.getDiagnostics().error(loc, instance->message);
    return;
  }
  const ClassDecl *classDecl = instance->cls;
  auto bindings =
      GenericCheckHelpers::bindingNames(classDecl->generic()->typeArgs, classDecl->typeArgs);
  auto message = ErrorTextInstanceUse::cannotUse(classDecl->name, bindings, instance->message);
  this->state.getDiagnostics().error(loc, message);
  this->noteFailedChain(instance);
}

void GenericClassChecker::noteFailedChain(const InstanceRecord *start) {
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

std::string GenericClassChecker::failedCallNote(const InstanceRecord *at,
                                                const InstanceRecord *next) const {
  llvm::StringRef name = GenericCheckHelpers::recordName(at);
  if (next == nullptr) {
    return ErrorTextInstanceUse::here(name);
  }

  return ErrorTextInstanceUse::usesHere(name, GenericCheckHelpers::recordName(next));
}

GenericClassChecker::ResultClassDeclared
GenericClassChecker::declaredFirst(const ClassDecl *generic) const {
  const auto &modules = this->state.getModuleStates();
  auto *loading = modules.getLoadingModule();
  bool isLoadingHome = loading != nullptr && modules.findDeclModule(generic) == loading;
  bool isEarly =
      !generic->declared && isLoadingHome && !this->state.getFunctionStack().isInsideFunction();
  if (isEarly) {
    auto message = ErrorTextDeclaredFirst::before(generic->name);

    return err(GenericClassCheckerDeclaredFirstError{
        .code = GenericClassCheckerDeclaredFirstErrorCode::USED_BEFORE_DECLARATION,
        .message = message,
    });
  }

  return ok(generic);
}

GenericClassChecker::ResultClassConcrete
GenericClassChecker::concrete(const ClassDecl *generic) const {
  if (generic->isAbstract) {
    auto message = ErrorTextConcrete::abstract(generic->name);

    return err(GenericClassCheckerConcreteError{
        .code = GenericClassCheckerConcreteErrorCode::ABSTRACT_CLASS,
        .message = message,
    });
  }

  return ok(generic);
}

GenericClassChecker::ResultCtorWasPicked
GenericClassChecker::constructorFit(const NewExpr *expr, const ClassDecl *generic,
                                    llvm::ArrayRef<FunctionDecl *> constructors) {
  auto fit = std::vector<FunctionDecl *>();
  for (FunctionDecl *constructor : constructors) {
    if (this->isTakingCount(constructor->sym, expr->args.size())) {
      fit.push_back(constructor);
    }
  }
  if (fit.empty()) {
    auto message = ErrorTextConstructorFit::inferArgs(generic->name, true, expr->args.size());

    return err(GenericClassCheckerConstructorFitError{
        .code = GenericClassCheckerConstructorFitErrorCode::NO_CONSTRUCTOR_FITS,
        .message = message,
    });
  }
  if (fit.size() > 1) {
    auto message = ErrorTextConstructorFit::inferArgs(generic->name, false, expr->args.size());

    return err(GenericClassCheckerConstructorFitError{
        .code = GenericClassCheckerConstructorFitErrorCode::SEVERAL_CONSTRUCTORS_FIT,
        .message = message,
    });
  }

  return ok(fit[0]);
}

bool GenericClassChecker::isTakingCount(const FuncSymbol *function, size_t count) const {
  if (count < function->minArgs) {
    return false;
  }

  return function->hasRest || count <= function->params.size();
}

Expr *GenericClassChecker::checkGenericNew(NewExpr *expr, ClassDecl *generic,
                                           const Type *expected) {
  expr->type = Type::error();
  auto *library = this->checkLibraryGenericNew(expr, generic, expected);
  if (library != nullptr) {
    return library;
  }
  if (!expr->typeArgs.empty()) {
    return this->checkExplicitGenericNew(expr, generic, expected);
  }
  auto declared = this->declaredFirst(generic);
  if (declared.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), declared.error().message);
  }
  auto concreteClass = this->concrete(generic);
  if (concreteClass.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), concreteClass.error().message);
  }
  auto constructors = this->classLinks->constructorsOf(generic);
  auto constructor = this->pickedConstructor(expr, generic, constructors);
  if (!constructor.has_value()) {
    this->walkArgsLoosely(expr->args);

    return expr;
  }
  auto *function = this->constructorFunction(generic, *constructor);
  auto *call = this->state.getAst().make<CallExpr>(expr->loc(), nullptr, std::move(expr->args));
  expr->form = ClassNew{.ctor = call};
  auto bound = std::vector<const Type *>();
  bool isBound =
      this->typeArgInferrer->bindGenericCall(call, function, expr->loc(), expected, {}, bound);
  if (!isBound) {
    return expr;
  }
  auto *instance = this->instantiatedClass(expr, generic, bound);
  if (instance == nullptr) {
    return expr;
  }

  return this->finishGenericNew(expr, instance, call, *constructor, constructors);
}

void GenericClassChecker::walkArgsLoosely(std::vector<Expr *> &args) {
  for (Expr *&arg : args) {
    if (arg->type == nullptr) {
      arg = this->exprChecker->checkExpr(arg);
    }
  }
}

Expr *GenericClassChecker::checkLibraryGenericNew(NewExpr *expr, ClassDecl *generic,
                                                  const Type *expected) {
  if (!generic->library()) {
    return nullptr;
  }
  bool isEmitter = LibraryClassTable::isName(generic->name, LibraryClass::EVENT_EMITTER);
  auto *emitter =
      isEmitter ? this->emitterChecker->checkEmitterNew(expr, generic, expected) : nullptr;
  if (emitter != nullptr) {
    return emitter;
  }
  if (!this->chunkTypes->isStreamClass(generic)) {
    return nullptr;
  }

  return this->streamNewChecker->checkStreamNew(expr, generic, expected);
}

Expr *GenericClassChecker::checkExplicitGenericNew(NewExpr *expr, ClassDecl *generic,
                                                   const Type *expected) {
  auto args = std::vector<const Type *>();
  for (const TypeRef &ref : expr->typeArgs) {
    const auto *arg = this->typeResolver->resolveTypeOrVoid(ref);
    if (arg->isError()) {
      this->walkArgsLoosely(expr->args);

      return expr;
    }
    args.push_back(arg);
  }
  auto *instance = this->instantiatedClass(expr, generic, std::move(args));
  if (instance == nullptr) {
    this->walkArgsLoosely(expr->args);

    return expr;
  }
  expr->typeArgs.clear();

  return this->methodCallChecker->checkClassNew(expr, instance, expected);
}

ClassDecl *GenericClassChecker::instantiatedClass(const NewExpr *expr, ClassDecl *generic,
                                                  std::vector<const Type *> args) {
  auto made =
      this->genericClassInstantiator->instantiateClass(generic, std::move(args), expr->loc());
  if (made.isOk()) {
    return made.value();
  }
  auto code = made.error().code;
  if (code == GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
    return nullptr;
  }
  bool isEndless = code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES;
  llvm::SMLoc at = isEndless ? generic->loc() : expr->loc();
  this->state.getDiagnostics().error(at, made.error().message);

  return nullptr;
}

std::optional<FunctionDecl *>
GenericClassChecker::pickedConstructor(const NewExpr *expr, const ClassDecl *generic,
                                       llvm::ArrayRef<FunctionDecl *> constructors) {
  if (constructors.empty()) {
    return nullptr;
  }
  if (constructors.size() == 1) {
    return constructors[0];
  }
  auto picked = this->constructorFit(expr, generic, constructors);
  if (picked.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), picked.error().message);

    return std::nullopt;
  }

  return picked.value();
}

FuncSymbol *GenericClassChecker::constructorFunction(ClassDecl *generic,
                                                     FunctionDecl *constructor) {
  auto *known = this->state.getGenericStore().findConstructorFunction(generic, constructor);
  if (known != nullptr) {
    return known;
  }
  auto *function = this->state.getAst().newFunc();
  function->setCompleter(this->state.getCompleter());
  this->state.getGenericStore().rememberConstructorFunction(generic, constructor, function);
  function->name = this->state.getAst().keepName("new " + generic->name.str());
  function->loc = generic->loc();
  auto *functionDecl = this->state.getAst().make<FunctionDecl>(generic->loc(), function->name);
  functionDecl->typeParams = generic->typeParams;
  functionDecl->sym = function;
  function->decl = functionDecl;
  function->typeParams = generic->typeArgs;
  function->typeDefaults = this->state.getGenericStore().getClassDefaults(generic);
  if (constructor != nullptr) {
    function->params = constructor->sym->params;
    function->minArgs = constructor->sym->minArgs;
    function->hasRest = constructor->sym->hasRest;
  }
  function->setReturnType(generic->type);

  return function;
}

Expr *GenericClassChecker::finishGenericNew(NewExpr *expr, ClassDecl *instance, CallExpr *call,
                                            FunctionDecl *constructor,
                                            llvm::ArrayRef<FunctionDecl *> constructors) {
  this->classDeclarer->completeClass(instance);
  expr->form = ClassNew{.cls = instance, .ctor = call};
  call->type = Type::voidTy();
  if (constructor != nullptr) {
    this->linkInstanceConstructor(expr, instance, call, constructor, constructors);
  }
  if (this->hasInitializer(instance)) {
    this->flowChecker->calledFunction();
  }
  expr->type = instance->type;

  return expr;
}

void GenericClassChecker::linkInstanceConstructor(const NewExpr *expr, ClassDecl *instance,
                                                  CallExpr *call, FunctionDecl *constructor,
                                                  llvm::ArrayRef<FunctionDecl *> constructors) {
  auto own = this->classLinks->constructorsOf(instance);
  auto position = size_t(llvm::find(constructors, constructor) - constructors.begin());
  if (position < own.size()) {
    call->target = FunctionCall{.func = own[position]->sym};
  }
  llvm::StringRef what = ObjectMemberTable::getName(ObjectMember::CONSTRUCTOR);
  auto accessible =
      this->classMemberChecker->checkAccess(constructor->cls(), constructor->access(), what);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), accessible.error().message);
  }
}

bool GenericClassChecker::hasInitializer(const ClassDecl *instance) const {
  const ClassDecl *classDecl = instance;
  while (classDecl != nullptr) {
    if (classDecl->init != nullptr || !classDecl->constructors.empty()) {
      return true;
    }
    classDecl = classDecl->parent;
  }

  return false;
}

}
