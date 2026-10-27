#include "compiler/check/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/check/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/check/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/check/generics/inference/inference.h"
#include "compiler/check/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

constexpr unsigned kMaxInstanceChain = 16;

const ClassDecl *literalClassIn(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen) {
  switch (type->kind()) {
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return literalClassIn(type->inner(), seen);
  case Type::Kind::Result:
    if (const ClassDecl *cls = literalClassIn(type->okType(), seen)) {
      return cls;
    }
    return literalClassIn(type->errType(), seen);
  case Type::Kind::Function:
    for (const Type *p : type->params()) {
      if (const ClassDecl *cls = literalClassIn(p, seen)) {
        return cls;
      }
    }
    return literalClassIn(type->ret(), seen);
  case Type::Kind::Iterator:
    for (const Type *t : {type->yielded(), type->returned(), type->sent()}) {
      if (const ClassDecl *cls = literalClassIn(t, seen)) {
        return cls;
      }
    }
    return nullptr;
  case Type::Kind::Dict:
    if (const ClassDecl *cls = literalClassIn(type->keyType(), seen)) {
      return cls;
    }
    return literalClassIn(type->valueType(), seen);
  case Type::Kind::Object:
    if (!seen.insert(type).second) {
      return nullptr;
    }
    if (const ClassDecl *cls = type->classDecl()) {
      return cls->literal ? cls : nullptr;
    }
    for (const Field &f : type->fields()) {
      if (const ClassDecl *cls = literalClassIn(f.type, seen)) {
        return cls;
      }
    }
    return nullptr;
  default:
    return nullptr;
  }
}

std::string typeList(llvm::ArrayRef<const Type *> types) {
  std::string text;
  for (const Type *t : types) {
    text += (text.empty() ? "" : ", ") + t->name().str();
  }

  return text;
}

std::string bindingList(llvm::ArrayRef<const Type *> params, llvm::ArrayRef<const Type *> args) {
  std::string text;
  for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
    text += (text.empty() ? "" : ", ") + params[i]->name().str() + " = " + args[i]->name().str();
  }

  return text;
}

}

GenericInstantiator::GenericInstantiator(Checker &checker) : checker_(checker) {}

Expr *GenericInstantiator::checkGenericValue(NameExpr *name, FuncSymbol *generic,
                                             const Checker::Signature *expected) {
  name->type = Type::error();
  checker_.returnInferrer().resolveParams(generic);
  const Type *ret = checker_.returnInferrer().returnType(generic);
  if (returnsOwnClass(generic, ret)) {
    ret = nullptr;
  }
  llvm::ArrayRef<const Type *> typeParams = generic->typeParams;
  std::vector<const Type *> bound(typeParams.size(), nullptr);
  if (!name->typeArgs.empty()) {
    if (name->typeArgs.size() > typeParams.size()) {
      checker_.diags_.error(name->loc(), "'" + generic->name + "' takes " +
                                             std::to_string(typeParams.size()) + " type argument" +
                                             (typeParams.size() == 1 ? "" : "s") + ", but got " +
                                             std::to_string(name->typeArgs.size()));
      return name;
    }
    for (size_t k = 0; k < name->typeArgs.size(); ++k) {
      bound[k] = checker_.typeResolver().resolveType(name->typeArgs[k], false);
    }
  } else if (expected) {
    Inference fromParams(typeParams, checker_.aliasOrigins_, nullptr),
        fromResult(typeParams, checker_.aliasOrigins_, nullptr);
    for (size_t i = 0; i < generic->params.size() && i < expected->params.size(); ++i) {
      if (expected->params[i]) {
        fromParams.from(generic->params[i], expected->params[i]);
      }
    }
    if (ret && expected->ret && !expected->voidResult) {
      fromResult.from(ret, expected->ret);
    }
    for (size_t k = 0; k < typeParams.size(); ++k) {
      for (const Inference *inference : {&fromParams, &fromResult}) {
        const Type *type = nullptr;
        for (const Type *c : inference->candidates[k]) {
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
  } else {
    checker_.diags_.error(name->loc(),
                          "'" + generic->name +
                              "' is generic, so a value of it needs its type arguments, "
                              "e.g. '" +
                              generic->name + "<int>', or a function type where it goes");
    return name;
  }
  if (!checker_.typeArgInferrer().finishTypeArgs(generic, bound, name->loc(), false) || !ret) {
    return name;
  }
  std::vector<const Type *> params;
  for (const Type *p : generic->params) {
    params.push_back(checker_.typeParams().substitute(p, typeParams, bound));
  }
  name->func = generic;
  name->type =
      Type::function(params, generic->minArgs, generic->hasRest,
                     checker_.typeParams().substitute(ret, typeParams, bound), generic->hasThis);
  bool concrete = llvm::none_of(bound, [](const Type *t) { return mentionsParam(t); });
  if (concrete && !checker_.trial_ && !generic->inferring) {
    if (FuncSymbol *instance = instantiate(generic, bound, name->loc())) {
      name->func = instance;
    }
  }

  return name;
}

bool GenericInstantiator::returnsOwnClass(const FuncSymbol *generic, const Type *ret) {
  llvm::SmallPtrSet<const Type *, 4> seen;
  const ClassDecl *cls = ret ? literalClassIn(ret, seen) : nullptr;
  if (!cls) {
    return false;
  }
  if (ownClassReturns_.insert(generic).second) {
    checker_.diags_.error(cls->literal->loc(),
                          "generic function '" + generic->name +
                              "' cannot return an object literal with methods yet: each copy of "
                              "it makes a class of its own; declare a class, or give the "
                              "object function fields, 'f: () => ...'");
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
    if (!param || !param->isConstructor() || !mentionsAnyOf(param, generic->typeParams)) {
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
      checker_.constructorCasts_.erase(conv);
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
    reps.push_back(checker_.typeParams().exactTypeParam(generic, k)
                       ? args[k]
                       : TypeParams::representation(args[k]));
  }
  std::vector<const Type *> keyTypes = reps;
  if (!exact.empty()) {
    keyTypes.push_back(nullptr);
    keyTypes.insert(keyTypes.end(), exact.begin(), exact.end());
  }
  auto key = std::make_pair(static_cast<const FuncSymbol *>(generic), keyTypes);
  if (auto found = instances_.find(key); found != instances_.end()) {
    Checker::Instance *instance = found->second;
    if (checker_.checkingInstance_ && !instance->checked && !instance->checking) {
      checkInstance(instance);
    }
    if (checker_.checkingInstance_ && instance->failed) {
      reportInstance(instance, loc, args);
    }
    return instance->decl->sym;
  }
  unsigned chain = 0;
  for (const Checker::Instance *i = checker_.checkingInstance_; i; i = i->requester) {
    chain += i->generic == generic;
  }
  if (chain >= kMaxInstanceChain) {
    checker_.diags_.error(
        loc, "calling '" + generic->name +
                 "' here needs another copy of it for each copy of the "
                 "functions that lead here, without end: its type arguments keep growing ('" +
                 typeList(args) +
                 "' by now); generic functions that call each other must pass "
                 "their type parameters on unchanged");
    return nullptr;
  }

  for (const Type *t : generic->params) {
    if (t && checker_.typeParams().substitute(t, generic->typeParams, reps)->isError()) {
      if (!checker_.checkingInstance_) {
        checker_.diags_.error(loc, "cannot call '" + generic->name + "' with " +
                                       bindingList(generic->typeParams, args) +
                                       ": the types of its parameters, '" + t->name() +
                                       "', mean nothing for these types");
      }
      return nullptr;
    }
  }
  Checker::Instance &instance = checker_.instanceStore_.emplace_back();
  instance.generic = generic;
  instance.args = reps;
  instance.concrete.assign(args.begin(), args.end());
  instance.exact.assign(exact.begin(), exact.end());
  instance.callLoc = loc;
  instance.requester = checker_.checkingInstance_;
  instance.decl = instanceDecl(generic, reps, exact);
  instances_[key] = &instance;
  if (ClassDecl *cls = generic->decl->cls; cls && !generic->decl->isStatic) {
    cls->methodInstances.push_back(instance.decl);
  } else {
    checker_.file_->instances.push_back(instance.decl);
  }
  if (checker_.lateClasses().isTemplate(generic)) {
    checker_.lateClasses().checkTemplateInstance(&instance);
    if (instance.failed) {
      reportInstance(&instance, loc, args);
    }
    return instance.decl->sym;
  }
  if (checker_.checkingInstance_) {
    checkInstance(&instance);
    if (instance.failed) {
      reportInstance(&instance, loc, args);
    }
  } else {
    checker_.pendingInstances_.push_back(&instance);
  }

  return instance.decl->sym;
}

FunctionDecl *GenericInstantiator::instanceDecl(FuncSymbol *generic,
                                                llvm::ArrayRef<const Type *> args,
                                                llvm::ArrayRef<const Type *> exact) {
  FunctionDecl *original = generic->decl;
  FunctionDecl *copy = checker_.returnInferrer().freshCopy(original);
  copy->isInstance = true;
  copy->typeArgs.assign(args.begin(), args.end());
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  FuncSymbol *sym = checker_.ctx_.newFunc();
  sym->name = generic->name;
  sym->loc = generic->loc;
  sym->decl = copy;
  for (const Type *p : generic->params) {
    sym->params.push_back(checker_.typeParams().substitute(p, params, args));
  }
  sym->minArgs = generic->minArgs;
  sym->hasRest = generic->hasRest;
  sym->ret = generic->ret ? checker_.typeParams().substitute(generic->ret, params, args) : nullptr;
  sym->predicateParam = generic->predicateParam;
  if (generic->predicateType) {
    sym->predicateType = checker_.typeParams().substitute(generic->predicateType, params, args);
  }
  for (size_t i = 0; i < copy->params.size() && i < original->params.size(); ++i) {
    copy->params[i].declaredType =
        checker_.typeParams().substitute(original->params[i].declaredType, params, args);
  }
  for (size_t i = 0; i < exact.size() && i < sym->params.size() && i < copy->params.size(); ++i) {
    if (exact[i]) {
      sym->params[i] = copy->params[i].declaredType = exact[i];
    }
  }
  copy->sym = sym;
  checker_.argListChecker().expandArgumentList(copy, sym);
  if (Module *home = checker_.declModules_.lookup(original)) {
    checker_.declModules_[copy] = home;
  }

  return copy;
}

void GenericInstantiator::checkInstance(Checker::Instance *instance) {
  instance->checking = true;
  std::vector<Diagnostics::Captured> errors;
  if (!checker_.brokenGenerics_.contains(instance->generic)) {
    Checker::BodyState saved = checker_.returnInferrer().enterBody(instance->decl);
    checker_.checkingInstance_ = instance;
    checker_.diags_.setCapture(&errors);
    checker_.flow_ = Checker::Flow();
    checker_.functionChecker().checkFunctionBody(instance->decl);
    checker_.returnInferrer().leaveBody(saved);
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
  FunctionDecl *copy = instanceDecl(instance->generic, instance->concrete, instance->exact);
  Checker::BodyState saved = checker_.returnInferrer().enterBody(copy);
  checker_.checkingInstance_ = instance;
  checker_.diags_.setCapture(&precise);
  checker_.flow_ = Checker::Flow();
  checker_.functionChecker().checkFunctionBody(copy);
  checker_.returnInferrer().leaveBody(saved);
  if (!precise.empty()) {
    instance->message = precise.front().message;
    instance->errorLoc = precise.front().loc;
  }
}

void GenericInstantiator::reportInstance(Checker::Instance *instance, llvm::SMLoc callLoc,
                                         llvm::ArrayRef<const Type *> concrete) {
  if (checker_.diags_.capture()) {
    checker_.failedCalls_[callLoc.getPointer()] = instance;
    checker_.diags_.error(callLoc, instance->message);
    return;
  }
  const FuncSymbol *generic = instance->generic;
  checker_.diags_.error(callLoc, "cannot call '" + generic->name + "' with " +
                                     bindingList(generic->typeParams, concrete) + ": " +
                                     instance->message);
  auto nameOf = [](const Checker::Instance *i) { return i->cls ? i->cls->name : i->generic->name; };
  const Checker::Instance *at = instance;
  llvm::SmallPtrSet<const char *, 8> shown;
  while (at && at->errorLoc.isValid() && shown.insert(at->errorLoc.getPointer()).second) {
    const Checker::Instance *next = checker_.failedCalls_.lookup(at->errorLoc.getPointer());
    if (next) {
      checker_.diags_.note(at->errorLoc, "in '" + nameOf(at) + "', which " +
                                             (next->cls ? "uses '" : "calls '") + nameOf(next) +
                                             "' here");
    } else {
      checker_.diags_.note(at->errorLoc, "in '" + nameOf(at) + "', here");
    }
    at = next;
  }
}

void GenericInstantiator::checkInstances() {
  for (size_t i = 0; i < checker_.pendingInstances_.size(); ++i) {
    Checker::Instance *instance = checker_.pendingInstances_[i];
    if (instance->checked) {
      continue;
    }
    if (instance->cls) {
      checker_.genericClassChecker().checkClassInstance(instance);
      if (instance->failed) {
        checker_.genericClassChecker().reportClassInstance(instance, instance->callLoc);
      }
      continue;
    }
    checkInstance(instance);
    if (instance->failed) {
      reportInstance(instance, instance->callLoc, instance->concrete);
    }
  }
  checker_.pendingInstances_.clear();
}

}
