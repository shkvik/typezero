#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/classes/overload_resolver/overload_resolver.h"
#include "compiler/check/errors/throw_checker/throw_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/check/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/events/event_target_checker/event_target_checker.h"
#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/library/stream/stream_checker/stream_checker.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

MethodCallChecker::MethodCallChecker(Sema &sema) : sema_(sema) {}

Expr *MethodCallChecker::checkMethodCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  ClassDecl *cls = ClassDeclarer::classOf(member->object->type);
  sema_.classDeclarer().completeClass(cls);
  call->type = Type::error();
  member->type = Type::error();
  llvm::StringRef name = member->name;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
  };
  if (cls->builtin && derivesFromError(cls)) {
    if (Expr *checked = sema_.throwChecker().checkErrorMethod(call, member)) {
      return checked;
    }
  }
  if (Expr *checked = sema_.fsDeclarer().checkFsClassMethod(call, member, cls)) {
    return checked;
  }
  if (Expr *checked = sema_.streamChecker().checkStreamCall(call, member, cls)) {
    return checked;
  }
  if (sema_.eventMaps().isEmitterClass(cls)) {
    if (Expr *checked = sema_.emitterChecker().checkEmitterCall(call, member, cls, /*super=*/false)) {
      return checked;
    }
  }
  if (ClassDecl *target = sema_.libraryClass("EventTarget"); target && cls->derivesFrom(target)) {
    if (Expr *checked = sema_.eventTargetChecker().checkEventTargetCall(call, member, cls)) {
      return checked;
    }
  }
  const ClassField *field = ClassBases::findField(cls, name);
  if (field && !field->method) {
    // A function in a field.
    call->callee = sema_.classMemberChecker().checkClassMember(member, nullptr);
    const Type *type = call->callee->type;
    if (type->isFunction()) {
      return sema_.callChecker().checkValueCall(call, type);
    }
    if (type->isOptional() && type->inner()->isFunction()) {
      sema_.diags_.error(member->loc(), "the function may be undefined; check it first, or call it with "
                                  "'?.()', which gives undefined then");
    } else if (!type->isError()) {
      sema_.diags_.error(member->loc(),
                   "field '" + name + "' is not a function; it has type '" + type->name() + "'");
    }
    checkArgsLoosely();
    return call;
  }
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, /*isStatic=*/false);
  if (methods.empty()) {
    if (!ClassDeclarer::methodsNamed(cls, name, /*isStatic=*/true).empty()) {
      sema_.diags_.error(member->loc(), "'" + name + "' is a static method of '" + cls->name +
                                      "': call it through the class, '" + cls->name + "." + name +
                                      "(...)'");
    } else {
      sema_.diags_.error(member->loc(),
                   "property '" + name + "' does not exist on type '" + cls->type->name() + "'");
    }
    checkArgsLoosely();
    return call;
  }
  sema_.classMemberChecker().checkAccess(methods[0]->cls, methods[0]->access, name, member->loc());
  if (sema_.classChecker().isInitThis(member->object) && !sema_.flow_.beforeSuper && !sema_.classChecker().thisReady(sema_.flow_)) {
    sema_.diags_.error(member->loc(),
                 "'this." + name + "()' is called before every field is set: field '" +
                     sema_.classChecker().firstUnset(sema_.flow_) + "' is not set yet, and the method may read it");
  }
  bool checked = false;
  std::vector<const Type *> bound;
  FuncSymbol *sym =
      sema_.overloadResolver().chooseVariant(methods, call->args, call->spread, member->loc(), checked, &bound);
  if (!sym || sema_.tzModulesChecker().rejectAccessorCall(call, member, sym)) {
    return call;
  }
  sema_.returnInferrer().resolveParams(sym);
  if (!sym->isGeneric() && !member->typeArgs.empty()) {
    sema_.diags_.error(member->loc(),
                 "'" + name + "' is not a generic method; remove the type arguments");
  }
  // A generic method: a call of the copy made for its types, which no class
  // overrides.
  if (sym->isGeneric() && checked) {
    return sema_.overloadResolver().callGenericVariant(call, sym, bound, member->loc(), /*method=*/true);
  }
  if (sym->isGeneric()) {
    sema_.typeArgInferrer().checkGenericCall(call, sym, member->loc(), expected, member->typeArgs);
    call->method = call->func;
    call->func = nullptr;
    if (call->method) {
      member->type = sema_.returnInferrer().functionType(call->method);
    }
    return call;
  }
  // A direct call, unless classes down the chain override the method: then
  // the method table of the object's class says which one runs.
  if (FunctionDecl *impl = ClassLinks::soleImplementation(cls, sym->decl)) {
    call->method = impl->sym;
  } else {
    call->method = sym, call->virtualCall = true;
  }
  member->type = sema_.returnInferrer().functionType(sym);
  call->type = sema_.returnInferrer().returnType(sym);
  // The arguments chooseVariant checked are only converted.
  std::vector<bool> given(checked ? call->args.size() : 0, true);

  return sema_.argumentChecker().checkArguments(call, sym->params, sym->minArgs, sym->hasRest, member->loc(), &given);
}

Expr *MethodCallChecker::checkClassNew(NewExpr *expr, ClassDecl *cls, const Type *expected) {
  expr->type = Type::error();
  if (cls->isGeneric()) {
    return sema_.genericClassChecker().checkGenericNew(expr, cls, expected);
  }
  // As node's `new AbortSignal()`, which throws "Illegal constructor".
  if (cls == sema_.libraryClass("AbortSignal") && !sema_.libraryCalls().inLibrary(expr->loc())) {
    sema_.diags_.error(expr->loc(), "an AbortSignal is not made with 'new'; take the signal of a "
                              "controller, 'new AbortController().signal', or use "
                              "'AbortSignal.timeout(ms)', 'AbortSignal.abort()' or "
                              "'AbortSignal.any(signals)'");
    for (Expr *&arg : expr->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    return expr;
  }
  if (sema_.fsDeclarer().isRuntimeClass(cls)) {
    std::string hint = cls == sema_.systemErrorClass_ || cls == sema_.execSyncErrorClass_
                           ? "; throw an Error, or an object of a class of your own that extends "
                             "Error"
                           : "";
    sema_.diags_.error(expr->loc(),
                 "'new " + cls->name + "' is not possible: only the functions of " +
                     std::string(cls == sema_.execSyncErrorClass_ ? "node:child_process" : "node:fs") +
                     " make " + cls->name + hint);
    for (Expr *&arg : expr->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    return expr;
  }
  expr->cls = cls;
  if (!expr->typeArgs.empty()) {
    sema_.diags_.error(expr->loc(),
                 "class '" + cls->name + "' is not generic; remove the type arguments");
  }
  if (!cls->staticOwner()->declared && sema_.loading_ && sema_.functions_.empty() &&
      sema_.declModules_.lookup(cls) == sema_.loading_) {
    sema_.diags_.error(expr->loc(), "class '" + cls->name +
                                  "' is used before its declaration, which JavaScript has not run "
                                  "yet here (a ReferenceError); move the class up");
  }
  if (cls->isAbstract) {
    sema_.diags_.error(expr->loc(), "class '" + cls->name +
                                  "' is abstract and cannot be created: create "
                                  "an object of a class that extends it");
  }
  sema_.classDeclarer().completeClass(cls);
  auto *call = sema_.ctx_.make<CallExpr>(expr->loc(), nullptr, std::move(expr->args));
  expr->ctor = call;
  call->type = Type::voidTy();
  // Without a constructor of its own, a class takes those of the nearest
  // class up its chain that has any, as in JavaScript.
  std::vector<FunctionDecl *> ctors = ClassLinks::constructorsOf(cls);
  if (ctors.empty()) {
    if (!call->args.empty()) {
      sema_.diags_.error(expr->loc(), "class '" + cls->name + "' has no constructor, so 'new " +
                                    cls->name + "()' takes no arguments");
      for (Expr *&arg : call->args) {
        arg = sema_.exprChecker().checkExpr(arg);
      }
      return expr;
    }
  } else {
    sema_.classMemberChecker().checkAccess(ctors[0]->cls, ctors[0]->access, "constructor", expr->loc());
    bool checked = false;
    FuncSymbol *sym = sema_.overloadResolver().chooseVariant(ctors, call->args, {}, expr->loc(), checked);
    if (!sym) {
      return expr;
    }
    sema_.returnInferrer().resolveParams(sym);
    call->func = sym;
    std::vector<bool> given(checked ? call->args.size() : 0, true);
    sema_.argumentChecker().checkArguments(call, sym->params, sym->minArgs, sym->hasRest, expr->loc(), &given);
  }
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (c->init || !c->constructors.empty()) {
      sema_.flowChecker().calledFunction(); // the code of the class runs
      break;
    }
  }
  expr->type = cls->type;

  return expr;
}

} // namespace tz
