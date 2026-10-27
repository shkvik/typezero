#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/classes/overload_resolver/overload_resolver.h"
#include "compiler/check/errors/throw_checker/throw_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/check/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/library/events/event_target_checker/event_target_checker.h"
#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/library/stream/stream_checker/stream_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

MethodCallChecker::MethodCallChecker(Checker &checker) : checker_(checker) {}

Expr *MethodCallChecker::checkMethodCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  ClassDecl *cls = ClassDeclarer::classOf(member->object->type);
  checker_.classDeclarer().completeClass(cls);
  call->type = Type::error();
  member->type = Type::error();
  llvm::StringRef name = member->name;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  if (cls->builtin && derivesFromError(cls)) {
    if (Expr *checked = checker_.throwChecker().checkErrorMethod(call, member)) {
      return checked;
    }
  }
  if (Expr *checked = checker_.fsDeclarer().checkFsClassMethod(call, member, cls)) {
    return checked;
  }
  if (Expr *checked = checker_.streamChecker().checkStreamCall(call, member, cls)) {
    return checked;
  }
  if (checker_.eventMaps().isEmitterClass(cls)) {
    if (Expr *checked = checker_.emitterChecker().checkEmitterCall(call, member, cls, false)) {
      return checked;
    }
  }
  if (ClassDecl *target = checker_.libraryClass("EventTarget");
      target && cls->derivesFrom(target)) {
    if (Expr *checked = checker_.eventTargetChecker().checkEventTargetCall(call, member, cls)) {
      return checked;
    }
  }
  const ClassField *field = ClassBases::findField(cls, name);
  if (field && !field->method) {
    call->callee = checker_.classMemberChecker().checkClassMember(member, nullptr);
    const Type *type = call->callee->type;
    if (type->isFunction()) {
      return checker_.callChecker().checkValueCall(call, type);
    }
    if (type->isOptional() && type->inner()->isFunction()) {
      checker_.diags_.error(member->loc(),
                            "the function may be undefined; check it first, or call it with "
                            "'?.()', which gives undefined then");
    } else if (!type->isError()) {
      checker_.diags_.error(member->loc(), "field '" + name + "' is not a function; it has type '" +
                                               type->name() + "'");
    }
    checkArgsLoosely();
    return call;
  }
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, false);
  if (methods.empty()) {
    if (!ClassDeclarer::methodsNamed(cls, name, true).empty()) {
      checker_.diags_.error(member->loc(), "'" + name + "' is a static method of '" + cls->name +
                                               "': call it through the class, '" + cls->name + "." +
                                               name + "(...)'");
    } else {
      checker_.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                               cls->type->name() + "'");
    }
    checkArgsLoosely();
    return call;
  }
  checker_.classMemberChecker().checkAccess(methods[0]->cls, methods[0]->access, name,
                                            member->loc());
  if (checker_.classChecker().isInitThis(member->object) && !checker_.flow_.beforeSuper &&
      !checker_.classChecker().thisReady(checker_.flow_)) {
    checker_.diags_.error(member->loc(), "'this." + name +
                                             "()' is called before every field is set: field '" +
                                             checker_.classChecker().firstUnset(checker_.flow_) +
                                             "' is not set yet, and the method may read it");
  }
  bool checked = false;
  std::vector<const Type *> bound;
  FuncSymbol *sym = checker_.overloadResolver().chooseVariant(methods, call->args, call->spread,
                                                              member->loc(), checked, &bound);
  if (!sym || checker_.tzModulesChecker().rejectAccessorCall(call, member, sym)) {
    return call;
  }
  checker_.returnInferrer().resolveParams(sym);
  if (!sym->isGeneric() && !member->typeArgs.empty()) {
    checker_.diags_.error(member->loc(),
                          "'" + name + "' is not a generic method; remove the type arguments");
  }
  if (sym->isGeneric() && checked) {
    return checker_.overloadResolver().callGenericVariant(call, sym, bound, member->loc(), true);
  }
  if (sym->isGeneric()) {
    checker_.typeArgInferrer().checkGenericCall(call, sym, member->loc(), expected,
                                                member->typeArgs);
    call->method = call->func;
    call->func = nullptr;
    if (call->method) {
      member->type = checker_.returnInferrer().functionType(call->method);
    }
    return call;
  }
  if (FunctionDecl *impl = ClassLinks::soleImplementation(cls, sym->decl)) {
    call->method = impl->sym;
  } else {
    call->method = sym, call->virtualCall = true;
  }
  member->type = checker_.returnInferrer().functionType(sym);
  call->type = checker_.returnInferrer().returnType(sym);
  std::vector<bool> given(checked ? call->args.size() : 0, true);

  return checker_.argumentChecker().checkArguments(call, sym->params, sym->minArgs, sym->hasRest,
                                                   member->loc(), &given);
}

Expr *MethodCallChecker::checkClassNew(NewExpr *expr, ClassDecl *cls, const Type *expected) {
  expr->type = Type::error();
  if (cls->isGeneric()) {
    return checker_.genericClassChecker().checkGenericNew(expr, cls, expected);
  }
  if (cls == checker_.libraryClass("AbortSignal") &&
      !checker_.libraryCalls().inLibrary(expr->loc())) {
    checker_.diags_.error(expr->loc(),
                          "an AbortSignal is not made with 'new'; take the signal of a "
                          "controller, 'new AbortController().signal', or use "
                          "'AbortSignal.timeout(ms)', 'AbortSignal.abort()' or "
                          "'AbortSignal.any(signals)'");
    for (Expr *&arg : expr->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return expr;
  }
  if (checker_.fsDeclarer().isRuntimeClass(cls)) {
    checker_.diags_.error(expr->loc(), "'new " + cls->name +
                                           "' is not possible: only the functions of node:fs make " +
                                           cls->name);
    for (Expr *&arg : expr->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return expr;
  }
  expr->cls = cls;
  if (!expr->typeArgs.empty()) {
    checker_.diags_.error(expr->loc(),
                          "class '" + cls->name + "' is not generic; remove the type arguments");
  }
  if (!cls->staticOwner()->declared && checker_.loading_ && checker_.functions_.empty() &&
      checker_.declModules_.lookup(cls) == checker_.loading_) {
    checker_.diags_.error(expr->loc(),
                          "class '" + cls->name +
                              "' is used before its declaration, which JavaScript has not run "
                              "yet here (a ReferenceError); move the class up");
  }
  if (cls->isAbstract) {
    checker_.diags_.error(expr->loc(), "class '" + cls->name +
                                           "' is abstract and cannot be created: create "
                                           "an object of a class that extends it");
  }
  checker_.classDeclarer().completeClass(cls);
  auto *call = checker_.ctx_.make<CallExpr>(expr->loc(), nullptr, std::move(expr->args));
  expr->ctor = call;
  call->type = Type::voidTy();
  std::vector<FunctionDecl *> ctors = ClassLinks::constructorsOf(cls);
  if (ctors.empty()) {
    if (!call->args.empty()) {
      checker_.diags_.error(expr->loc(), "class '" + cls->name + "' has no constructor, so 'new " +
                                             cls->name + "()' takes no arguments");
      for (Expr *&arg : call->args) {
        arg = checker_.exprChecker().checkExpr(arg);
      }
      return expr;
    }
  } else {
    checker_.classMemberChecker().checkAccess(ctors[0]->cls, ctors[0]->access, "constructor",
                                              expr->loc());
    bool checked = false;
    FuncSymbol *sym =
        checker_.overloadResolver().chooseVariant(ctors, call->args, {}, expr->loc(), checked);
    if (!sym) {
      return expr;
    }
    checker_.returnInferrer().resolveParams(sym);
    call->func = sym;
    std::vector<bool> given(checked ? call->args.size() : 0, true);
    checker_.argumentChecker().checkArguments(call, sym->params, sym->minArgs, sym->hasRest,
                                              expr->loc(), &given);
  }
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (c->init || !c->constructors.empty()) {
      checker_.flowChecker().calledFunction();
      break;
    }
  }
  expr->type = cls->type;

  return expr;
}

}
