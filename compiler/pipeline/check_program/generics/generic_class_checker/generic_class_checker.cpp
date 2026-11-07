#include "compiler/pipeline/check_program/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "compiler/pipeline/check_program/declare/class_declarer/class_declarer.h"
#include "compiler/pipeline/check_program/declare/module_declarer/module_declarer.h"
#include "compiler/pipeline/check_program/flow/flow_checker/flow_checker.h"
#include "compiler/pipeline/check_program/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/pipeline/check_program/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/pipeline/check_program/generics/type_params/type_params.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/topics/classes/class_links/class_links.h"
#include "compiler/pipeline/check_program/topics/classes/class_member_checker/class_member_checker.h"
#include "compiler/pipeline/check_program/topics/classes/method_call_checker/method_call_checker.h"
#include "compiler/pipeline/check_program/topics/events/emitter_checker/emitter_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/functions/function_checker/function_checker.h"
#include "compiler/pipeline/check_program/topics/stream/chunk_types/chunk_types.h"
#include "compiler/pipeline/check_program/topics/stream/stream_new_checker/stream_new_checker.h"
#include "compiler/pipeline/check_program/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

std::string GenericClassChecker::typeNames(llvm::ArrayRef<const Type *> types) {
  std::string text;
  for (const Type *t : types) {
    text += (text.empty() ? "" : ", ") + t->name().str();
  }

  return text;
}

std::string GenericClassChecker::bindings(llvm::ArrayRef<const Type *> params,
                                          llvm::ArrayRef<const Type *> args) {
  std::string text;
  for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
    text += (text.empty() ? "" : ", ") + params[i]->name().str() + " = " + args[i]->name().str();
  }

  return text;
}

const ClassDecl *GenericClassChecker::genericInstanceOf(const Type *type,
                                                        const ClassDecl *generic) {
  for (const ClassDecl *c = type ? type->classDecl() : nullptr; c; c = c->parent) {
    if (c == generic || c->generic() == generic) {
      return c;
    }
  }

  return nullptr;
}

GenericClassChecker::GenericClassChecker(CheckState &owner)
    : state(owner) {}

void GenericClassChecker::declareClassTypeParams(ClassDecl *cls) {
  if (!cls->isGeneric() || !cls->typeArgs.empty()) {
    return;
  }
  this->state.getScopeStack().enterOnlyTypes(this->state.getScopeStack().getTopTypes());
  Module *here = this->state.getModuleStates().getCurrentModule();
  Module *home = this->state.getModuleStates().findDeclModule(cls);
  bool moved = home && home != this->state.getModuleStates().getCurrentModule() &&
               this->state.getScopeStack().isTopLevel();
  if (moved) {
    ModuleDeclarer(this->state).useModule(home);
  }
  this->state.getScopeStack().openTypeScope();
  std::vector<const Type *> defaults;
  cls->typeArgs = TypeParams(this->state).declareTypeParams(cls->typeParams, &defaults);
  this->state.getScopeStack().closeTypeScope();
  if (moved) {
    ModuleDeclarer(this->state).useModule(here);
  }
  this->state.getScopeStack().leaveOnlyTypes();
  this->state.getGenericStore().rememberClassDefaults(cls, std::move(defaults));
  auto *type = const_cast<Type *>(cls->type);
  type->setTypeArgs(cls->typeArgs);
  type->rename(cls->name.str() + "<" + GenericClassChecker::typeNames(cls->typeArgs) + ">");
  this->state.getGenericStore().rememberClassInstance(cls, cls->typeArgs, cls);
}

void GenericClassChecker::bindClassTypeParams(const ClassDecl *cls) {
  if (cls && !cls->typeParams.empty() && cls->typeParams.size() == cls->typeArgs.size()) {
    TypeParams(this->state).bindTypeParams(cls->typeParams, cls->typeArgs);
  }
}

const Type *GenericClassChecker::instanceType(const ClassDecl *inst, const Type *type) {
  if (!type || !inst->generic()) {
    return type;
  }

  return TypeParams(this->state).substitute(type, inst->generic()->typeArgs, inst->typeArgs);
}

const Type *GenericClassChecker::instanceValueType(const ClassDecl *inst, const Type *type) {
  if (!type || !inst->generic()) {
    return type;
  }

  return TypeParams(this->state).substituteValue(type, inst->generic()->typeArgs, inst->typeArgs);
}

void GenericClassChecker::checkClassInstance(InstanceRecord *instance) {
  ClassDecl *inst = instance->cls;
  if (instance->checked) {
    return;
  }
  instance->checking = true;
  std::vector<Diagnostics::Captured> errors;
  if (!this->state.getClassStore().isBrokenClass(inst->generic())) {
    ClassDeclarer(this->state).completeClass(inst);
    std::vector<FunctionDecl *> bodies;
    if (inst->init) {
      bodies.push_back(inst->init);
    }
    bodies.insert(bodies.end(), inst->constructors.begin(), inst->constructors.end());
    for (FunctionDecl *fn : ClassLinks::ownMethods(inst)) {
      if (!fn->isAbstract() && fn->typeParams.empty()) {
        bodies.push_back(fn);
      }
    }
    for (FunctionDecl *fn : bodies) {
      if (fn->checked || !fn->sym) {
        continue;
      }
      if (!fn->sym->ret) {
        ReturnInferrer(this->state).inferReturnType(fn->sym);
      }
      BodyState saved = ReturnInferrer(this->state).enterBody(fn);
      this->state.getGenericStore().rememberCheckingInstance(instance);
      this->state.getDiagnostics().setCapture(&errors);
      this->state.getFlowState().replaceFlow(Flow());
      FunctionChecker(this->state).checkFunctionBody(fn);
      ReturnInferrer(this->state).leaveBody(saved);
    }
  }
  instance->checking = false;
  instance->checked = true;
  if (errors.empty() || instance->failed) {
    return;
  }
  instance->failed = true;
  instance->message = errors.front().message;
  instance->errorLoc = errors.front().loc;
}

void GenericClassChecker::reportClassInstance(InstanceRecord *instance, llvm::SMLoc use) {
  llvm::SMLoc loc = use.isValid() ? use : instance->cls->generic()->loc();
  if (this->state.getDiagnostics().capture()) {
    this->state.getGenericStore().rememberFailedCall(loc, instance);
    this->state.getDiagnostics().error(loc, instance->message);
    return;
  }
  const ClassDecl *inst = instance->cls;
  std::string bindings = GenericClassChecker::bindings(inst->generic()->typeArgs, inst->typeArgs);
  std::string message = ErrorTextInstanceUse::cannotUse(inst->name, bindings, instance->message);
  this->state.getDiagnostics().error(loc, message);
  const InstanceRecord *at = instance;
  llvm::SmallPtrSet<const char *, 8> shown;
  auto nameOf = [](const InstanceRecord *i) { return i->cls ? i->cls->name : i->generic->name; };
  while (at && at->errorLoc.isValid() && shown.insert(at->errorLoc.getPointer()).second) {
    const InstanceRecord *next = this->state.getGenericStore().findFailedCall(at->errorLoc);
    std::string where = next ? ErrorTextInstanceUse::usesHere(nameOf(at), nameOf(next))
                             : ErrorTextInstanceUse::here(nameOf(at));
    this->state.getDiagnostics().note(at->errorLoc, where);
    at = next;
  }
}

GenericClassChecker::ResultClassDeclared
GenericClassChecker::declaredFirst(const ClassDecl *generic) const {
  bool early = !generic->declared && this->state.getModuleStates().getLoadingModule() &&
               !this->state.getFunctionStack().isInsideFunction() &&
               this->state.getModuleStates().findDeclModule(generic) ==
                   this->state.getModuleStates().getLoadingModule();
  if (early) {
    std::string message = ErrorTextDeclaredFirst::before(generic->name);

    return err(GenericClassCheckerDeclaredFirstError{
        .code = GenericClassCheckerDeclaredFirstErrorCode::USED_BEFORE_DECLARATION,
        .message = message,
    });
  }

  return ok(generic);
}

GenericClassChecker::ResultClassConcrete GenericClassChecker::concrete(const ClassDecl *generic) {
  if (generic->isAbstract) {
    std::string message = ErrorTextConcrete::abstract(generic->name);

    return err(GenericClassCheckerConcreteError{
        .code = GenericClassCheckerConcreteErrorCode::ABSTRACT_CLASS,
        .message = message,
    });
  }

  return ok(generic);
}

GenericClassChecker::ResultCtorWasPicked
GenericClassChecker::constructorFit(const NewExpr *expr, const ClassDecl *generic,
                                    llvm::ArrayRef<FunctionDecl *> ctors) {
  std::vector<FunctionDecl *> fit;
  for (FunctionDecl *c : ctors) {
    ReturnInferrer(this->state).resolveParams(c->sym);
    const FuncSymbol *sym = c->sym;
    if (expr->args.size() >= sym->minArgs &&
        (sym->hasRest || expr->args.size() <= sym->params.size())) {
      fit.push_back(c);
    }
  }
  if (fit.empty()) {
    std::string message =
        ErrorTextConstructorFit::inferArgs(generic->name, true, expr->args.size());

    return err(GenericClassCheckerConstructorFitError{
        .code = GenericClassCheckerConstructorFitErrorCode::NO_CONSTRUCTOR_FITS,
        .message = message,
    });
  }
  if (fit.size() > 1) {
    std::string message =
        ErrorTextConstructorFit::inferArgs(generic->name, false, expr->args.size());

    return err(GenericClassCheckerConstructorFitError{
        .code = GenericClassCheckerConstructorFitErrorCode::SEVERAL_CONSTRUCTORS_FIT,
        .message = message,
    });
  }

  return ok(fit[0]);
}

Expr *GenericClassChecker::checkGenericNew(NewExpr *expr, ClassDecl *generic,
                                           const Type *expected) {
  expr->type = Type::error();
  auto checkArgsLoosely = [&](std::vector<Expr *> &args) {
    for (Expr *&arg : args) {
      if (!arg->type) {
        arg = ExprChecker(this->state).checkExpr(arg);
      }
    }
  };
  if (generic->library() && generic->name == "EventEmitter") {
    if (Expr *emitter = EmitterChecker(this->state).checkEmitterNew(expr, generic, expected)) {
      return emitter;
    }
  }
  if (generic->library() && ChunkTypes(this->state).isStreamClass(generic)) {
    if (Expr *stream = StreamNewChecker(this->state).checkStreamNew(expr, generic, expected)) {
      return stream;
    }
  }
  if (!expr->typeArgs.empty()) {
    std::vector<const Type *> args;
    for (const TypeRef &ref : expr->typeArgs) {
      args.push_back(TypeResolver(this->state).resolveType(ref, true));
      if (args.back()->isError()) {
        checkArgsLoosely(expr->args);

        return expr;
      }
    }
    GenericClassInstantiator::ResultClassMade made =
        GenericClassInstantiator(this->state)
            .instantiateClass(generic, std::move(args), expr->loc());
    if (made.isErr() &&
        made.error().code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
      llvm::SMLoc at =
          made.error().code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
              ? generic->loc()
              : expr->loc();
      this->state.getDiagnostics().error(at, made.error().message);
    }
    ClassDecl *inst = made.isOk() ? made.value() : nullptr;
    if (!inst) {
      checkArgsLoosely(expr->args);
      return expr;
    }
    expr->typeArgs.clear();
    return MethodCallChecker(this->state).checkClassNew(expr, inst, expected);
  }
  ResultClassDeclared declared = this->declaredFirst(generic);
  if (declared.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), declared.error().message);
  }
  ResultClassConcrete concreteClass = GenericClassChecker::concrete(generic);
  if (concreteClass.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), concreteClass.error().message);
  }
  std::vector<FunctionDecl *> ctors = ClassLinks::constructorsOf(generic);
  FunctionDecl *ctor = ctors.size() == 1 ? ctors[0] : nullptr;
  if (ctors.size() > 1) {
    ResultCtorWasPicked picked = this->constructorFit(expr, generic, ctors);
    if (picked.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), picked.error().message);
      checkArgsLoosely(expr->args);
      return expr;
    }
    ctor = picked.value();
  }
  FuncSymbol *fn = this->state.getGenericStore().findConstructorFunction(generic, ctor);
  if (!fn) {
    fn = this->state.getAst().newFunc();
    this->state.getGenericStore().rememberConstructorFunction(generic, ctor, fn);
    fn->name = this->state.getAst().keepName("new " + generic->name.str());
    fn->loc = generic->loc();
    auto *decl = this->state.getAst().make<FunctionDecl>(generic->loc(), fn->name);
    decl->typeParams = generic->typeParams;
    decl->sym = fn;
    fn->decl = decl;
    fn->typeParams = generic->typeArgs;
    fn->typeDefaults = this->state.getGenericStore().getClassDefaults(generic);
    if (ctor) {
      ReturnInferrer(this->state).resolveParams(ctor->sym);
      fn->params = ctor->sym->params;
      fn->minArgs = ctor->sym->minArgs;
      fn->hasRest = ctor->sym->hasRest;
    }
    fn->ret = generic->type;
  }
  auto *call = this->state.getAst().make<CallExpr>(expr->loc(), nullptr, std::move(expr->args));
  expr->form = ClassNew{.ctor = call};
  std::vector<const Type *> bound;
  if (!TypeArgInferrer(this->state).bindGenericCall(call, fn, expr->loc(), expected, {}, bound)) {
    return expr;
  }
  GenericClassInstantiator::ResultClassMade made =
      GenericClassInstantiator(this->state).instantiateClass(generic, bound, expr->loc());
  if (made.isErr() &&
      made.error().code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
    llvm::SMLoc at =
        made.error().code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
            ? generic->loc()
            : expr->loc();
    this->state.getDiagnostics().error(at, made.error().message);
  }
  ClassDecl *inst = made.isOk() ? made.value() : nullptr;
  if (!inst) {
    return expr;
  }
  ClassDeclarer(this->state).completeClass(inst);
  expr->form = ClassNew{.cls = inst, .ctor = call};
  call->type = Type::voidTy();
  if (ctor) {
    std::vector<FunctionDecl *> mine = ClassLinks::constructorsOf(inst);
    auto at = llvm::find(ctors, ctor) - ctors.begin();
    if (size_t(at) < mine.size()) {
      call->target = FunctionCall{.func = mine[size_t(at)]->sym};
    }
    ClassMemberChecker::ResultMemberAccessible accessible =
        ClassMemberChecker(this->state).checkAccess(ctor->cls(), ctor->access(), "constructor");
    if (accessible.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), accessible.error().message);
    }
  }
  for (const ClassDecl *c = inst; c; c = c->parent) {
    if (c->init || !c->constructors.empty()) {
      FlowChecker(this->state).calledFunction();
      break;
    }
  }
  expr->type = inst->type;

  return expr;
}

}
