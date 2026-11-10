#include "compiler/semantic/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
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
  this->typeParams = &all.getTypeParams();
  this->lateClasses = &all.getLateClasses();
  this->typeResolver = &all.getTypeResolver();
  this->argListChecker = &all.getArgListChecker();
  this->returnInferrer = &all.getReturnInferrer();
  this->functionChecker = &all.getFunctionChecker();
  this->typeArgInferrer = &all.getTypeArgInferrer();
  this->genericClassChecker = &all.getGenericClassChecker();
}

const ClassDecl *GenericInstantiator::literalClassIn(const Type *type) {
  TypeParts parts = TypeParts::collect(type, kLiteralClassRoute);
  for (const Type *part : parts.getTypes()) {
    const ClassDecl *cls = part->classDecl();
    if (cls != nullptr && cls->literal()) {
      return cls;
    }
  }

  return nullptr;
}

std::string GenericInstantiator::typeList(llvm::ArrayRef<const Type *> types) {
  std::string text;
  for (const Type *t : types) {
    text += (text.empty() ? "" : ", ") + t->name().str();
  }

  return text;
}

std::string GenericInstantiator::bindingList(llvm::ArrayRef<const Type *> params,
                                             llvm::ArrayRef<const Type *> args) {
  std::string text;
  for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
    text += (text.empty() ? "" : ", ") + params[i]->name().str() + " = " + args[i]->name().str();
  }

  return text;
}

GenericInstantiator::ResultTypeArgsCount
GenericInstantiator::typeArgCount(const FuncSymbol *generic, const NameExpr *name) {
  if (name->typeArgs.size() > generic->typeParams.size()) {
    std::string message = ErrorTextGenericCount::tooMany(generic->name, generic->typeParams.size(),
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
                                   const Signature *expected) {
  if (name->typeArgs.empty() && !expected) {
    std::string message = ErrorTextGenericValue::needsArgs(generic->name);

    return err(GenericInstantiatorTypeArgsGivenError{
        .code = GenericInstantiatorTypeArgsGivenErrorCode::NEEDS_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(name);
}

GenericInstantiator::ResultNoClassReturn
GenericInstantiator::ownClassReturn(const FuncSymbol *generic, const ClassDecl *cls) {
  if (cls) {
    std::string message = ErrorTextOwnClass::literal(generic->name);

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
  unsigned chain = 0;
  for (const InstanceRecord *i = this->state.getGenericStore().getCheckingInstance(); i;
       i = i->requester) {
    chain += i->generic == generic;
  }
  if (chain >= kMaxInstanceChain) {
    std::string types = GenericInstantiator::typeList(args);
    std::string message = ErrorTextInstanceChain::endless(generic->name, types);

    return err(GenericInstantiatorChainBoundedError{
        .code = GenericInstantiatorChainBoundedErrorCode::ENDLESS_CHAIN,
        .message = message,
    });
  }

  return ok(chain);
}

GenericInstantiator::ResultParamsMeaning
GenericInstantiator::paramsMeaningful(const FuncSymbol *generic, llvm::ArrayRef<const Type *> reps,
                                      llvm::ArrayRef<const Type *> args) {
  for (const Type *t : generic->params) {
    if (t && this->typeParams->substitute(t, generic->typeParams, reps)->isError()) {
      std::string bindings = GenericInstantiator::bindingList(generic->typeParams, args);
      std::string message =
          ErrorTextInstanceParams::meaningless(generic->name, bindings, t->name());

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
  const Type *ret = generic->getReturnType();
  if (this->returnsOwnClass(generic, ret)) {
    ret = nullptr;
  }
  llvm::ArrayRef<const Type *> genericTypeParams = generic->typeParams;
  std::vector<const Type *> bound(genericTypeParams.size(), nullptr);
  ResultTypeArgsGiven given = GenericInstantiator::typeArgsGiven(generic, name, expected);
  if (given.isErr()) {
    this->state.getDiagnostics().error(name->loc(), given.error().message);
    return name;
  }
  if (!name->typeArgs.empty()) {
    ResultTypeArgsCount counted = GenericInstantiator::typeArgCount(generic, name);
    if (counted.isErr()) {
      this->state.getDiagnostics().error(name->loc(), counted.error().message);
      return name;
    }
    for (size_t k = 0; k < name->typeArgs.size(); ++k) {
      bound[k] = this->typeResolver->resolveType(name->typeArgs[k], true);
    }
  } else if (expected) {
    Inference fromParams(genericTypeParams, this->state.getAliasStore(), nullptr),
        fromResult(genericTypeParams, this->state.getAliasStore(), nullptr);
    for (size_t i = 0; i < generic->params.size() && i < expected->params.size(); ++i) {
      if (expected->params[i]) {
        fromParams.from(generic->params[i], expected->params[i]);
      }
    }
    if (ret && expected->ret && !expected->voidResult) {
      fromResult.from(ret, expected->ret);
    }
    for (size_t k = 0; k < genericTypeParams.size(); ++k) {
      for (const Inference *inference : {&fromParams, &fromResult}) {
        const Type *type = nullptr;
        for (const Type *c : inference->getCandidates(k)) {
          type = type ? TypeRelations::unify(type, c) : c;
          if (!type) {
            break;
          }
        }
        if (type && !bound[k]) {
          bound[k] = type;
        }
      }
    }
  }
  if (!this->typeArgInferrer->finishTypeArgs(generic, bound, name->loc(), false) || !ret) {
    return name;
  }
  std::vector<const Type *> params;
  for (const Type *p : generic->params) {
    params.push_back(this->typeParams->substituteValue(p, genericTypeParams, bound));
  }
  name->binding = NamedFunction{.func = generic};
  name->type =
      Type::function(params, generic->minArgs, generic->hasRest,
                     this->typeParams->substitute(ret, genericTypeParams, bound), generic->hasThis);
  bool concrete = llvm::none_of(bound, [](const Type *t) { return TypeQueries::mentionsParam(t); });
  if (concrete && !this->state.getFunctionStack().isInsideTrial() && !generic->inferring) {
    if (FuncSymbol *instance = this->instantiate(generic, bound, name->loc())) {
      name->binding = NamedFunction{.func = instance};
    }
  }

  return name;
}

bool GenericInstantiator::returnsOwnClass(const FuncSymbol *generic, const Type *ret) {
  const ClassDecl *cls = ret ? GenericInstantiator::literalClassIn(ret) : nullptr;
  ResultNoClassReturn plain = GenericInstantiator::ownClassReturn(generic, cls);
  if (plain.isOk()) {
    return false;
  }
  if (!this->state.getGenericStore().isOwnClassReturnReported(generic)) {
    this->state.getGenericStore().markOwnClassReturnReported(generic);
    this->state.getDiagnostics().error(cls->literal()->loc(), plain.error().message);
  }

  return true;
}

std::vector<const Type *> GenericInstantiator::exactClassArgs(CallExpr *call,
                                                              const FuncSymbol *generic) {
  std::vector<const Type *> exact;
  if (generic->hasRest) {
    return exact;
  }
  for (size_t i = 0; i < generic->params.size() && i < call->args.size(); ++i) {
    const Type *param = generic->params[i];
    if (!param || !param->isConstructor() ||
        !GenericCheckHelpers::mentionsAnyOf(param, generic->typeParams)) {
      continue;
    }
    Expr *arg = call->args[i];
    std::vector<const CastExpr *> dropped;
    while (auto *conv = dyn_cast<CastExpr>(arg)) {
      if (!conv->isImplicit()) {
        break;
      }
      dropped.push_back(conv);
      arg = conv->operand;
    }
    const ClassDecl *cls = arg->type->classRef();
    if (!cls || !cls->type) {
      continue;
    }
    exact.resize(generic->params.size(), nullptr);
    exact[i] = arg->type;
    call->args[i] = arg;
    for (const CastExpr *conv : dropped) {
      this->state.getReflectStore().forgetConstructorCast(conv);
    }
  }

  return exact;
}

FuncSymbol *GenericInstantiator::instantiate(FuncSymbol *generic, llvm::ArrayRef<const Type *> args,
                                             llvm::SMLoc loc, llvm::ArrayRef<const Type *> exact) {
  if (llvm::any_of(args, [](const Type *t) { return t->isError(); })) {
    return nullptr;
  }
  std::vector<const Type *> reps;
  for (size_t k = 0; k < args.size(); ++k) {
    reps.push_back(this->typeParams->exactTypeParam(generic, k)
                       ? args[k]
                       : TypeParams::representation(args[k]));
  }
  std::vector<const Type *> keyTypes = reps;
  if (!exact.empty()) {
    keyTypes.push_back(nullptr);
    keyTypes.insert(keyTypes.end(), exact.begin(), exact.end());
  }
  GenericStore &generics = this->state.getGenericStore();
  if (InstanceRecord *instance = generics.findFunctionInstance(generic, keyTypes)) {
    if (this->state.getGenericStore().isCheckingInstance() && !instance->checked &&
        !instance->checking) {
      this->checkInstance(instance);
    }
    if (this->state.getGenericStore().isCheckingInstance() && instance->failed) {
      this->reportInstance(instance, loc, args);
    }
    return instance->decl->sym;
  }
  ResultChainIsFinite bounded = this->chainBounded(generic, args);
  if (bounded.isErr()) {
    this->state.getDiagnostics().error(loc, bounded.error().message);
    return nullptr;
  }
  ResultParamsMeaning meaningful = this->paramsMeaningful(generic, reps, args);
  if (meaningful.isErr() && !this->state.getGenericStore().isCheckingInstance()) {
    this->state.getDiagnostics().error(loc, meaningful.error().message);
  }
  if (meaningful.isErr()) {
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
  if (ClassDecl *cls = generic->decl->cls(); cls && !generic->decl->isStatic()) {
    cls->methodInstances.push_back(instance.decl);
  } else {
    this->state.getInventoryStore().addInstance(instance.decl);
  }
  if (this->lateClasses->isTemplate(generic)) {
    this->lateClasses->checkTemplateInstance(&instance);
    if (instance.failed) {
      this->reportInstance(&instance, loc, args);
    }
    return instance.decl->sym;
  }
  if (this->state.getGenericStore().isCheckingInstance()) {
    this->checkInstance(&instance);
    if (instance.failed) {
      this->reportInstance(&instance, loc, args);
    }
  } else {
    generics.addPendingInstance(&instance);
  }

  return instance.decl->sym;
}

FunctionDecl *GenericInstantiator::instanceDecl(FuncSymbol *generic,
                                                llvm::ArrayRef<const Type *> args,
                                                llvm::ArrayRef<const Type *> exact) {
  FunctionDecl *original = generic->decl;
  FunctionDecl *copy = this->returnInferrer->freshCopy(original);
  copy->isInstance = true;
  copy->typeArgs.assign(args.begin(), args.end());
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  FuncSymbol *sym = this->state.getAst().newFunc();
  sym->setCompleter(this->state.getCompleter());
  sym->name = generic->name;
  sym->loc = generic->loc;
  sym->decl = copy;
  for (const Type *p : generic->params) {
    sym->params.push_back(this->typeParams->substituteValue(p, params, args));
  }
  sym->minArgs = generic->minArgs;
  sym->hasRest = generic->hasRest;
  const Type *returned =
      generic->hasReturnType()
          ? this->typeParams->substitute(generic->getKnownReturnType(), params, args)
          : nullptr;
  sym->setReturnType(returned);
  if (generic->predicate) {
    const Type *type = this->typeParams->substitute(generic->predicate->type, params, args);
    sym->predicate = TypePredicate{.param = generic->predicate->param, .type = type};
  }
  for (size_t i = 0; i < copy->params.size() && i < original->params.size(); ++i) {
    copy->params[i].declaredType =
        this->typeParams->substituteValue(original->params[i].declaredType, params, args);
  }
  for (size_t i = 0; i < exact.size() && i < sym->params.size() && i < copy->params.size(); ++i) {
    if (exact[i]) {
      sym->params[i] = copy->params[i].declaredType = exact[i];
    }
  }
  copy->sym = sym;
  this->argListChecker->expandArgumentList(copy, sym);
  if (Module *home = this->state.getModuleStates().findDeclModule(original)) {
    this->state.getModuleStates().rememberDeclModule(copy, home);
  }

  return copy;
}

void GenericInstantiator::checkInstance(InstanceRecord *instance) {
  instance->checking = true;
  std::vector<Diagnostics::Captured> errors;
  if (!this->state.getGenericStore().isBrokenGeneric(instance->generic)) {
    BodyState saved = this->returnInferrer->enterBody(instance->decl);
    this->state.getGenericStore().rememberCheckingInstance(instance);
    this->state.getDiagnostics().setCapture(&errors);
    this->functionChecker->walkFunctionFromScratch(instance->decl);
    this->returnInferrer->leaveBody(saved);
  }
  instance->checking = false;
  instance->checked = true;
  if (errors.empty()) {
    return;
  }
  instance->failed = true;
  instance->message = errors.front().message;
  instance->errorLoc = errors.front().loc;
  if (instance->concrete == instance->args) {
    return;
  }
  std::vector<Diagnostics::Captured> precise;
  FunctionDecl *copy = this->instanceDecl(instance->generic, instance->concrete, instance->exact);
  BodyState saved = this->returnInferrer->enterBody(copy);
  this->state.getGenericStore().rememberCheckingInstance(instance);
  this->state.getDiagnostics().setCapture(&precise);
  this->functionChecker->walkFunctionFromScratch(copy);
  this->returnInferrer->leaveBody(saved);
  if (!precise.empty()) {
    instance->message = precise.front().message;
    instance->errorLoc = precise.front().loc;
  }
}

void GenericInstantiator::reportInstance(InstanceRecord *instance, llvm::SMLoc callLoc,
                                         llvm::ArrayRef<const Type *> concrete) {
  if (this->state.getDiagnostics().capture()) {
    this->state.getGenericStore().rememberFailedCall(callLoc, instance);
    this->state.getDiagnostics().error(callLoc, instance->message);
    return;
  }
  const FuncSymbol *generic = instance->generic;
  std::string bindings = GenericInstantiator::bindingList(generic->typeParams, concrete);
  std::string message =
      ErrorTextInstanceCall::cannotCall(generic->name, bindings, instance->message);
  this->state.getDiagnostics().error(callLoc, message);
  auto nameOf = [](const InstanceRecord *i) { return i->cls ? i->cls->name : i->generic->name; };
  const InstanceRecord *at = instance;
  llvm::SmallPtrSet<const char *, 8> shown;
  while (at && at->errorLoc.isValid() && shown.insert(at->errorLoc.getPointer()).second) {
    const InstanceRecord *next = this->state.getGenericStore().findFailedCall(at->errorLoc);
    std::string where =
        next ? ErrorTextInstanceCall::inWhich(nameOf(at), next->cls != nullptr, nameOf(next))
             : ErrorTextInstanceCall::here(nameOf(at));
    this->state.getDiagnostics().note(at->errorLoc, where);
    at = next;
  }
}

void GenericInstantiator::checkInstances() {
  GenericStore &generics = this->state.getGenericStore();
  for (int i = 0; i < generics.countPendingInstances(); ++i) {
    InstanceRecord *instance = generics.getPendingInstance(i);
    if (instance->checked) {
      continue;
    }
    if (instance->cls) {
      this->genericClassChecker->checkClassInstance(instance);
      if (instance->failed) {
        this->genericClassChecker->reportClassInstance(instance, instance->callLoc);
      }
      continue;
    }
    this->checkInstance(instance);
    if (instance->failed) {
      this->reportInstance(instance, instance->callLoc, instance->concrete);
    }
  }
  generics.forgetPendingInstances();
}

}
