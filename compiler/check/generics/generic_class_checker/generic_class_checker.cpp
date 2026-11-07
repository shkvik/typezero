#include "compiler/check/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/check/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "compiler/library/stream/stream_new_checker/stream_new_checker.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
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

const ClassDecl *genericInstanceOf(const Type *type, const ClassDecl *generic) {
  for (const ClassDecl *c = type ? type->classDecl() : nullptr; c; c = c->parent) {
    if (c == generic || c->generic() == generic) {
      return c;
    }
  }

  return nullptr;
}

GenericClassChecker::GenericClassChecker(Checker &owner)
    : checker(owner) {}

void GenericClassChecker::declareClassTypeParams(ClassDecl *cls) {
  if (!cls->isGeneric() || !cls->typeArgs.empty()) {
    return;
  }
  std::vector<llvm::StringMap<Stmt *>> outer = this->checker.typeScopes_;
  this->checker.typeScopes_.resize(1);
  Module *here = this->checker.module_;
  Module *home = this->checker.declModules_.lookup(cls);
  bool moved = home && home != this->checker.module_ && this->checker.scopes_.size() == 1;
  if (moved) {
    this->checker.moduleDeclarer().useModule(home);
  }
  this->checker.typeScopes_.emplace_back();
  std::vector<const Type *> defaults;
  cls->typeArgs = this->checker.typeParams().declareTypeParams(cls->typeParams, &defaults);
  this->checker.typeScopes_.pop_back();
  if (moved) {
    this->checker.moduleDeclarer().useModule(here);
  }
  this->checker.typeScopes_ = std::move(outer);
  this->checker.classDefaults_[cls] = std::move(defaults);
  auto *type = const_cast<Type *>(cls->type);
  type->setTypeArgs(cls->typeArgs);
  type->rename(cls->name.str() + "<" + GenericClassChecker::typeNames(cls->typeArgs) + ">");
  this->checker.classInstances_[{cls, cls->typeArgs}] = cls;
}

void GenericClassChecker::bindClassTypeParams(const ClassDecl *cls) {
  if (cls && !cls->typeParams.empty() && cls->typeParams.size() == cls->typeArgs.size()) {
    this->checker.typeParams().bindTypeParams(cls->typeParams, cls->typeArgs);
  }
}

const Type *GenericClassChecker::instanceType(const ClassDecl *inst, const Type *type) {
  if (!type || !inst->generic()) {
    return type;
  }

  return this->checker.typeParams().substitute(type, inst->generic()->typeArgs, inst->typeArgs);
}

const Type *GenericClassChecker::instanceValueType(const ClassDecl *inst, const Type *type) {
  if (!type || !inst->generic()) {
    return type;
  }

  return this->checker.typeParams().substituteValue(type, inst->generic()->typeArgs,
                                                    inst->typeArgs);
}

void GenericClassChecker::checkClassInstance(Checker::Instance *instance) {
  ClassDecl *inst = instance->cls;
  if (instance->checked) {
    return;
  }
  instance->checking = true;
  std::vector<Diagnostics::Captured> errors;
  if (!this->checker.brokenClasses_.contains(inst->generic())) {
    this->checker.classDeclarer().completeClass(inst);
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
        this->checker.returnInferrer().inferReturnType(fn->sym);
      }
      Checker::BodyState saved = this->checker.returnInferrer().enterBody(fn);
      this->checker.checkingInstance_ = instance;
      this->checker.diags_.setCapture(&errors);
      this->checker.flow_ = Checker::Flow();
      this->checker.functionChecker().checkFunctionBody(fn);
      this->checker.returnInferrer().leaveBody(saved);
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

void GenericClassChecker::reportClassInstance(Checker::Instance *instance, llvm::SMLoc use) {
  llvm::SMLoc loc = use.isValid() ? use : instance->cls->generic()->loc();
  if (this->checker.diags_.capture()) {
    this->checker.failedCalls_[loc.getPointer()] = instance;
    this->checker.diags_.error(loc, instance->message);
    return;
  }
  const ClassDecl *inst = instance->cls;
  std::string bindings = GenericClassChecker::bindings(inst->generic()->typeArgs, inst->typeArgs);
  std::string message = ErrorTextInstanceUse::cannotUse(inst->name, bindings, instance->message);
  this->checker.diags_.error(loc, message);
  const Checker::Instance *at = instance;
  llvm::SmallPtrSet<const char *, 8> shown;
  auto nameOf = [](const Checker::Instance *i) { return i->cls ? i->cls->name : i->generic->name; };
  while (at && at->errorLoc.isValid() && shown.insert(at->errorLoc.getPointer()).second) {
    const Checker::Instance *next = this->checker.failedCalls_.lookup(at->errorLoc.getPointer());
    std::string where = next ? ErrorTextInstanceUse::usesHere(nameOf(at), nameOf(next))
                             : ErrorTextInstanceUse::here(nameOf(at));
    this->checker.diags_.note(at->errorLoc, where);
    at = next;
  }
}

GenericClassChecker::ResultClassDeclared
GenericClassChecker::declaredFirst(const ClassDecl *generic) const {
  bool early = !generic->declared && this->checker.loading_ && this->checker.functions_.empty() &&
               this->checker.declModules_.lookup(generic) == this->checker.loading_;
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
    this->checker.returnInferrer().resolveParams(c->sym);
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
        arg = this->checker.exprChecker().checkExpr(arg);
      }
    }
  };
  if (generic->library() && generic->name == "EventEmitter") {
    if (Expr *emitter = this->checker.emitterChecker().checkEmitterNew(expr, generic, expected)) {
      return emitter;
    }
  }
  if (generic->library() && this->checker.chunkTypes().isStreamClass(generic)) {
    if (Expr *stream = this->checker.streamNewChecker().checkStreamNew(expr, generic, expected)) {
      return stream;
    }
  }
  if (!expr->typeArgs.empty()) {
    std::vector<const Type *> args;
    for (const TypeRef &ref : expr->typeArgs) {
      args.push_back(this->checker.typeResolver().resolveType(ref, true));
      if (args.back()->isError()) {
        checkArgsLoosely(expr->args);

        return expr;
      }
    }
    GenericClassInstantiator::ResultClassMade made =
        this->checker.genericClassInstantiator().instantiateClass(generic, std::move(args),
                                                                  expr->loc());
    if (made.isErr() &&
        made.error().code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
      llvm::SMLoc at =
          made.error().code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
              ? generic->loc()
              : expr->loc();
      this->checker.diags_.error(at, made.error().message);
    }
    ClassDecl *inst = made.isOk() ? made.value() : nullptr;
    if (!inst) {
      checkArgsLoosely(expr->args);
      return expr;
    }
    expr->typeArgs.clear();
    return this->checker.methodCallChecker().checkClassNew(expr, inst, expected);
  }
  ResultClassDeclared declared = this->declaredFirst(generic);
  if (declared.isErr()) {
    this->checker.diags_.error(expr->loc(), declared.error().message);
  }
  ResultClassConcrete concreteClass = GenericClassChecker::concrete(generic);
  if (concreteClass.isErr()) {
    this->checker.diags_.error(expr->loc(), concreteClass.error().message);
  }
  std::vector<FunctionDecl *> ctors = ClassLinks::constructorsOf(generic);
  FunctionDecl *ctor = ctors.size() == 1 ? ctors[0] : nullptr;
  if (ctors.size() > 1) {
    ResultCtorWasPicked picked = this->constructorFit(expr, generic, ctors);
    if (picked.isErr()) {
      this->checker.diags_.error(expr->loc(), picked.error().message);
      checkArgsLoosely(expr->args);
      return expr;
    }
    ctor = picked.value();
  }
  FuncSymbol *&fn = this->constructorFunctions[{generic, ctor}];
  if (!fn) {
    fn = this->checker.ctx_.newFunc();
    fn->name = this->checker.ctx_.keepName("new " + generic->name.str());
    fn->loc = generic->loc();
    auto *decl = this->checker.ctx_.make<FunctionDecl>(generic->loc(), fn->name);
    decl->typeParams = generic->typeParams;
    decl->sym = fn;
    fn->decl = decl;
    fn->typeParams = generic->typeArgs;
    fn->typeDefaults = this->checker.classDefaults_.lookup(generic);
    if (ctor) {
      this->checker.returnInferrer().resolveParams(ctor->sym);
      fn->params = ctor->sym->params;
      fn->minArgs = ctor->sym->minArgs;
      fn->hasRest = ctor->sym->hasRest;
    }
    fn->ret = generic->type;
  }
  auto *call = this->checker.ctx_.make<CallExpr>(expr->loc(), nullptr, std::move(expr->args));
  expr->form = ClassNew{.ctor = call};
  std::vector<const Type *> bound;
  if (!this->checker.typeArgInferrer().bindGenericCall(call, fn, expr->loc(), expected, {},
                                                       bound)) {
    return expr;
  }
  GenericClassInstantiator::ResultClassMade made =
      this->checker.genericClassInstantiator().instantiateClass(generic, bound, expr->loc());
  if (made.isErr() &&
      made.error().code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
    llvm::SMLoc at =
        made.error().code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
            ? generic->loc()
            : expr->loc();
    this->checker.diags_.error(at, made.error().message);
  }
  ClassDecl *inst = made.isOk() ? made.value() : nullptr;
  if (!inst) {
    return expr;
  }
  this->checker.classDeclarer().completeClass(inst);
  expr->form = ClassNew{.cls = inst, .ctor = call};
  call->type = Type::voidTy();
  if (ctor) {
    std::vector<FunctionDecl *> mine = ClassLinks::constructorsOf(inst);
    auto at = llvm::find(ctors, ctor) - ctors.begin();
    if (size_t(at) < mine.size()) {
      call->target = FunctionCall{.func = mine[size_t(at)]->sym};
    }
    ClassMemberChecker::ResultMemberAccessible accessible =
        this->checker.classMemberChecker().checkAccess(ctor->cls(), ctor->access(), "constructor");
    if (accessible.isErr()) {
      this->checker.diags_.error(expr->loc(), accessible.error().message);
    }
  }
  for (const ClassDecl *c = inst; c; c = c->parent) {
    if (c->init || !c->constructors.empty()) {
      this->checker.flowChecker().calledFunction();
      break;
    }
  }
  expr->type = inst->type;

  return expr;
}

}
