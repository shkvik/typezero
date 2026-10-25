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

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

// "int, string"
std::string typeNames(llvm::ArrayRef<const Type *> types) {
  std::string text;
  for (const Type *t : types) {
    text += (text.empty() ? "" : ", ") + t->name().str();
  }

  return text;
}

// "T = int, U = string"
std::string bindings(llvm::ArrayRef<const Type *> params, llvm::ArrayRef<const Type *> args) {
  std::string text;
  for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
    text += (text.empty() ? "" : ", ") + params[i]->name().str() + " = " + args[i]->name().str();
  }

  return text;
}

} // namespace

const ClassDecl *genericInstanceOf(const Type *type, const ClassDecl *generic) {
  for (const ClassDecl *c = type ? type->classDecl() : nullptr; c; c = c->parent) {
    if (c == generic || c->generic == generic) {
      return c;
    }
  }

  return nullptr;
}

GenericClassChecker::GenericClassChecker(Checker &checker) : checker_(checker) {}

void GenericClassChecker::declareClassTypeParams(ClassDecl *cls) {
  if (!cls->isGeneric() || !cls->typeArgs.empty()) {
    return;
  }
  // In the file of the class, where the names its constraints use mean what
  // they mean there, and without the type parameters of any code around.
  std::vector<llvm::StringMap<Stmt *>> outer = checker_.typeScopes_;
  checker_.typeScopes_.resize(1);
  Module *here = checker_.module_;
  Module *home = checker_.declModules_.lookup(cls);
  bool moved = home && home != checker_.module_ && checker_.scopes_.size() == 1;
  if (moved) {
    checker_.moduleDeclarer().useModule(home);
  }
  checker_.typeScopes_.emplace_back();
  std::vector<const Type *> defaults;
  cls->typeArgs = checker_.typeParams().declareTypeParams(cls->typeParams, &defaults);
  checker_.typeScopes_.pop_back();
  if (moved) {
    checker_.moduleDeclarer().useModule(here);
  }
  checker_.typeScopes_ = std::move(outer);
  checker_.classDefaults_[cls] = std::move(defaults);
  // `Box<T>` in messages, and the type of `this` in its methods.
  auto *type = const_cast<Type *>(cls->type);
  type->setTypeArgs(cls->typeArgs);
  type->rename(cls->name.str() + "<" + typeNames(cls->typeArgs) + ">");
  checker_.classInstances_[{cls, cls->typeArgs}] = cls;
}

void GenericClassChecker::bindClassTypeParams(const ClassDecl *cls) {
  if (cls && !cls->typeParams.empty() && cls->typeParams.size() == cls->typeArgs.size()) {
    checker_.typeParams().bindTypeParams(cls->typeParams, cls->typeArgs);
  }
}

const Type *GenericClassChecker::instanceType(const ClassDecl *inst, const Type *type) {
  if (!type || !inst->generic) {
    return type;
  }

  return checker_.typeParams().substitute(type, inst->generic->typeArgs, inst->typeArgs);
}

void GenericClassChecker::checkClassInstance(Checker::Instance *instance) {
  ClassDecl *inst = instance->cls;
  if (instance->checked) {
    return;
  }
  instance->checking = true;
  std::vector<Diagnostics::Captured> errors;
  if (!checker_.brokenClasses_.contains(inst->generic)) {
    checker_.classDeclarer().completeClass(inst);
    std::vector<FunctionDecl *> bodies;
    if (inst->init) {
      bodies.push_back(inst->init);
    }
    bodies.insert(bodies.end(), inst->constructors.begin(), inst->constructors.end());
    // A generic method is checked in the copies of it that calls need.
    for (FunctionDecl *fn : ClassLinks::ownMethods(inst)) {
      if (!fn->isAbstract && fn->typeParams.empty()) {
        bodies.push_back(fn);
      }
    }
    for (FunctionDecl *fn : bodies) {
      if (fn->checked || !fn->sym) {
        continue;
      }
      if (!fn->sym->ret) {
        checker_.returnInferrer().inferReturnType(fn->sym); // that of the generic method
      }
      Checker::BodyState saved = checker_.returnInferrer().enterBody(fn);
      checker_.checkingInstance_ = instance;
      checker_.diags_.setCapture(&errors);
      checker_.flow_ = Checker::Flow();
      checker_.functionChecker().checkFunctionBody(fn);
      checker_.returnInferrer().leaveBody(saved);
    }
  }
  instance->checking = false;
  instance->checked = true;
  // The variants that the types make the same are reported first (see
  // fillInstance).
  if (errors.empty() || instance->failed) {
    return;
  }
  instance->failed = true;
  instance->message = errors.front().message;
  instance->errorLoc = errors.front().loc;
}

void GenericClassChecker::reportClassInstance(Checker::Instance *instance, llvm::SMLoc use) {
  llvm::SMLoc loc = use.isValid() ? use : instance->cls->generic->loc();
  if (checker_.diags_.capture()) {
    // In the body of another instance: this use is where that one fails.
    checker_.failedCalls_[loc.getPointer()] = instance;
    checker_.diags_.error(loc, instance->message);
    return;
  }
  const ClassDecl *inst = instance->cls;
  checker_.diags_.error(loc, "cannot use '" + inst->name + "' with " +
                                 bindings(inst->generic->typeArgs, inst->typeArgs) + ": " +
                                 instance->message);
  const Checker::Instance *at = instance;
  llvm::SmallPtrSet<const char *, 8> shown;
  auto nameOf = [](const Checker::Instance *i) { return i->cls ? i->cls->name : i->generic->name; };
  while (at && at->errorLoc.isValid() && shown.insert(at->errorLoc.getPointer()).second) {
    const Checker::Instance *next = checker_.failedCalls_.lookup(at->errorLoc.getPointer());
    if (next) {
      checker_.diags_.note(at->errorLoc,
                           "in '" + nameOf(at) + "', which uses '" + nameOf(next) + "' here");
    } else {
      checker_.diags_.note(at->errorLoc, "in '" + nameOf(at) + "', here");
    }
    at = next;
  }
}

Expr *GenericClassChecker::checkGenericNew(NewExpr *expr, ClassDecl *generic,
                                           const Type *expected) {
  expr->type = Type::error();
  auto checkArgsLoosely = [&](std::vector<Expr *> &args) {
    for (Expr *&arg : args) {
      if (!arg->type) {
        arg = checker_.exprChecker().checkExpr(arg);
      }
    }
  };
  // `new EventEmitter()`, with its event map or without (sema_events.cpp).
  if (generic->library && generic->name == "EventEmitter") {
    if (Expr *emitter = checker_.emitterChecker().checkEmitterNew(expr, generic, expected)) {
      return emitter;
    }
  }
  // `new Readable({...})` (sema_stream.cpp).
  if (generic->library && checker_.chunkTypes().isStreamClass(generic)) {
    if (Expr *stream = checker_.streamNewChecker().checkStreamNew(expr, generic, expected)) {
      return stream;
    }
  }
  // `new Box<int>(1)`: the instance, as any class.
  if (!expr->typeArgs.empty()) {
    std::vector<const Type *> args;
    for (const TypeRef &ref : expr->typeArgs) {
      args.push_back(checker_.typeResolver().resolveType(ref, /*allowVoid=*/false));
      if (args.back()->isError()) {
        checkArgsLoosely(expr->args);

        return expr;
      }
    }
    ClassDecl *inst =
        checker_.genericClassInstantiator().instantiateClass(generic, std::move(args), expr->loc());
    if (!inst) {
      checkArgsLoosely(expr->args);
      return expr;
    }
    expr->typeArgs.clear();
    return checker_.methodCallChecker().checkClassNew(expr, inst, expected);
  }
  if (!generic->declared && checker_.loading_ && checker_.functions_.empty() &&
      checker_.declModules_.lookup(generic) == checker_.loading_) {
    checker_.diags_.error(expr->loc(),
                          "class '" + generic->name +
                              "' is used before its declaration, which JavaScript has not run "
                              "yet here (a ReferenceError); move the class up");
  }
  if (generic->isAbstract) {
    checker_.diags_.error(expr->loc(), "class '" + generic->name +
                                           "' is abstract and cannot be created: create "
                                           "an object of a class that extends it");
  }
  // The type arguments come from the constructor that the arguments choose,
  // as from a generic function whose result is `Box<T>`.
  std::vector<FunctionDecl *> ctors = ClassLinks::constructorsOf(generic);
  FunctionDecl *ctor = ctors.size() == 1 ? ctors[0] : nullptr;
  if (ctors.size() > 1) {
    std::vector<FunctionDecl *> fit;
    for (FunctionDecl *c : ctors) {
      checker_.returnInferrer().resolveParams(c->sym);
      const FuncSymbol *sym = c->sym;
      if (expr->args.size() >= sym->minArgs &&
          (sym->hasRest || expr->args.size() <= sym->params.size())) {
        fit.push_back(c);
      }
    }
    if (fit.size() != 1) {
      checker_.diags_.error(
          expr->loc(), "cannot infer the type arguments of '" + generic->name + "': " +
                           (fit.empty() ? "no constructor takes " : "several constructors take ") +
                           std::to_string(expr->args.size()) + " argument" +
                           (expr->args.size() == 1 ? "" : "s") + "; give them, e.g. 'new " +
                           generic->name + "<int>(...)'");
      checkArgsLoosely(expr->args);
      return expr;
    }
    ctor = fit[0];
  }
  FuncSymbol *&fn = constructorFunctions_[{generic, ctor}];
  if (!fn) {
    fn = checker_.ctx_.newFunc();
    fn->name = checker_.ctx_.keepName("new " + generic->name.str());
    fn->loc = generic->loc();
    auto *decl = checker_.ctx_.make<FunctionDecl>(generic->loc(), fn->name);
    decl->typeParams = generic->typeParams;
    decl->sym = fn;
    fn->decl = decl;
    fn->typeParams = generic->typeArgs;
    fn->typeDefaults = checker_.classDefaults_.lookup(generic);
    if (ctor) {
      checker_.returnInferrer().resolveParams(ctor->sym);
      fn->params = ctor->sym->params;
      fn->minArgs = ctor->sym->minArgs;
      fn->hasRest = ctor->sym->hasRest;
    }
    fn->ret = generic->type;
  }
  auto *call = checker_.ctx_.make<CallExpr>(expr->loc(), nullptr, std::move(expr->args));
  expr->ctor = call;
  std::vector<const Type *> bound;
  if (!checker_.typeArgInferrer().bindGenericCall(call, fn, expr->loc(), expected, {}, bound)) {
    return expr;
  }
  ClassDecl *inst =
      checker_.genericClassInstantiator().instantiateClass(generic, bound, expr->loc());
  if (!inst) {
    return expr;
  }
  checker_.classDeclarer().completeClass(inst);
  expr->cls = inst;
  call->type = Type::voidTy();
  // The constructor of the instance that is a copy of the one chosen: the
  // same parameters, with the types of the instance.
  if (ctor) {
    std::vector<FunctionDecl *> mine = ClassLinks::constructorsOf(inst);
    auto at = llvm::find(ctors, ctor) - ctors.begin();
    if (size_t(at) < mine.size()) {
      call->func = mine[size_t(at)]->sym;
    }
    checker_.classMemberChecker().checkAccess(ctor->cls, ctor->access, "constructor", expr->loc());
  }
  for (const ClassDecl *c = inst; c; c = c->parent) {
    if (c->init || !c->constructors.empty()) {
      checker_.flowChecker().calledFunction(); // the code of the class runs
      break;
    }
  }
  expr->type = inst->type;

  return expr;
}

} // namespace tz
