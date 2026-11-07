#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/classes/overload_resolver/overload_resolver.h"
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


namespace tz {

MethodCallChecker::MethodCallChecker(Checker &owner)
    : checker(owner) {}

Expr *MethodCallChecker::checkMethodCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  ClassDecl *cls = ClassDeclarer::classOf(member->object->type);
  this->checker.classDeclarer().completeClass(cls);
  this->checker.tzModulesChecker().noteTlsMethod(member, cls);
  call->type = Type::error();
  member->type = Type::error();
  llvm::StringRef name = member->name;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
  };
  if (Expr *checked = this->checker.fsDeclarer().checkFsClassMethod(call, member, cls)) {
    return checked;
  }
  if (Expr *checked = this->checker.streamChecker().checkStreamCall(call, member, cls)) {
    return checked;
  }
  if (this->checker.eventMaps().isEmitterClass(cls)) {
    if (Expr *checked = this->checker.emitterChecker().checkEmitterCall(call, member, cls, false)) {
      return checked;
    }
  }
  if (ClassDecl *target = this->checker.libraryClass("EventTarget");
      target && cls->derivesFrom(target)) {
    if (Expr *checked =
            this->checker.eventTargetChecker().checkEventTargetCall(call, member, cls)) {
      return checked;
    }
  }
  const ClassField *field = ClassBases::findField(cls, name);
  if (field && !field->method()) {
    call->callee = this->checker.classMemberChecker().checkClassMember(member, nullptr);
    const Type *type = call->callee->type;
    if (type->isFunction()) {
      return this->checker.callChecker().checkValueCall(call, type);
    }
    if (type->isOptional() && type->inner()->isFunction()) {
      this->checker.diags_.error(member->loc(),
                                 "the function may be undefined; check it first, or call it with "
                                 "'?.()', which gives undefined then");
    } else if (!type->isError()) {
      this->checker.diags_.error(member->loc(), "field '" + name +
                                                    "' is not a function; it has type '" +
                                                    type->name() + "'");
    }
    checkArgsLoosely();
    return call;
  }
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, false);
  if (methods.empty()) {
    if (!ClassDeclarer::methodsNamed(cls, name, true).empty()) {
      this->checker.diags_.error(member->loc(), "'" + name + "' is a static method of '" +
                                                    cls->name + "': call it through the class, '" +
                                                    cls->name + "." + name + "(...)'");
    } else {
      this->checker.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                                    cls->type->name() + "'");
    }
    checkArgsLoosely();
    return call;
  }
  if (methods.size() == 1) {
    this->checker.classMemberChecker().checkAccess(methods[0]->cls(), methods[0]->access(), name,
                                                   member->loc(), member->object);
  }
  if (this->checker.classChecker().isInitThis(member->object).isOk() &&
      !this->checker.flow_.beforeSuper &&
      this->checker.classChecker().thisReady(this->checker.flow_).isErr()) {
    this->checker.diags_.error(
        member->loc(), "'this." + name + "()' is called before every field is set: field '" +
                           this->checker.classChecker().firstUnset(this->checker.flow_).value() +
                           "' is not set yet, and the method may read it");
  }
  bool checked = false;
  std::vector<const Type *> bound;
  FuncSymbol *sym = this->checker.overloadResolver().chooseVariant(
      methods, call->args, call->spread, member->loc(), checked, &bound);
  if (!sym) {
    return call;
  }
  if (methods.size() > 1 && sym->decl) {
    this->checker.classMemberChecker().checkAccess(sym->decl->cls(), sym->decl->access(), name,
                                                   member->loc(), member->object);
  }
  this->checker.returnInferrer().resolveParams(sym);
  if (!sym->isGeneric() && !member->typeArgs.empty()) {
    this->checker.diags_.error(member->loc(),
                               "'" + name + "' is not a generic method; remove the type arguments");
  }
  if (sym->isGeneric() && checked) {
    return this->checker.overloadResolver().callGenericVariant(call, sym, bound, member->loc(),
                                                               true);
  }
  if (sym->isGeneric()) {
    this->checker.typeArgInferrer().checkGenericCall(call, sym, member->loc(), expected,
                                                     member->typeArgs);
    call->target = MethodCall{.method = call->func()};
    if (call->method()) {
      member->type = this->checker.returnInferrer().functionType(call->method());
    }
    return call;
  }
  if (FunctionDecl *impl = ClassLinks::soleImplementation(cls, sym->decl)) {
    call->target = MethodCall{.method = impl->sym};
  } else {
    call->target = MethodCall{.method = sym, .virtualCall = true};
  }
  member->type = this->checker.returnInferrer().functionType(sym);
  call->type = this->checker.returnInferrer().returnType(sym);
  std::vector<bool> given(checked ? call->args.size() : 0, true);

  return this->checker.argumentChecker().checkArguments(call, sym->params, sym->minArgs,
                                                        sym->hasRest, member->loc(), &given);
}

Expr *MethodCallChecker::checkClassNew(NewExpr *expr, ClassDecl *cls, const Type *expected) {
  expr->type = Type::error();
  if (cls->isGeneric()) {
    return this->checker.genericClassChecker().checkGenericNew(expr, cls, expected);
  }
  if (this->checker.fsDeclarer().isRuntimeClass(cls)) {
    this->checker.diags_.error(
        expr->loc(),
        "'new " + cls->name + "' is not possible: only the functions of node:fs make " + cls->name);
    for (Expr *&arg : expr->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
    return expr;
  }
  if (!expr->typeArgs.empty()) {
    this->checker.diags_.error(expr->loc(), "class '" + cls->name +
                                                "' is not generic; remove the type arguments");
  }
  if (!cls->staticOwner()->declared && this->checker.loading_ && this->checker.functions_.empty() &&
      this->checker.declModules_.lookup(cls) == this->checker.loading_) {
    this->checker.diags_.error(expr->loc(),
                               "class '" + cls->name +
                                   "' is used before its declaration, which JavaScript has not run "
                                   "yet here (a ReferenceError); move the class up");
  }
  if (cls->isAbstract) {
    this->checker.diags_.error(expr->loc(), "class '" + cls->name +
                                                "' is abstract and cannot be created: create "
                                                "an object of a class that extends it");
  }
  this->checker.classDeclarer().completeClass(cls);
  auto *call = this->checker.ctx_.make<CallExpr>(expr->loc(), nullptr, std::move(expr->args));
  expr->form = ClassNew{.cls = cls, .ctor = call};
  call->type = Type::voidTy();
  std::vector<FunctionDecl *> ctors = ClassLinks::constructorsOf(cls);
  if (ctors.empty()) {
    if (!call->args.empty()) {
      this->checker.diags_.error(expr->loc(), "class '" + cls->name +
                                                  "' has no constructor, so 'new " + cls->name +
                                                  "()' takes no arguments");
      for (Expr *&arg : call->args) {
        arg = this->checker.exprChecker().checkExpr(arg);
      }
      return expr;
    }
  } else {
    bool checked = false;
    FuncSymbol *sym =
        this->checker.overloadResolver().chooseVariant(ctors, call->args, {}, expr->loc(), checked);
    if (!sym) {
      return expr;
    }
    const FunctionDecl *chosen = sym->decl ? sym->decl : ctors[0];
    this->checker.classMemberChecker().checkAccess(chosen->cls(), chosen->access(), "constructor",
                                                   expr->loc());
    this->checker.returnInferrer().resolveParams(sym);
    call->target = FunctionCall{.func = sym};
    std::vector<bool> given(checked ? call->args.size() : 0, true);
    this->checker.argumentChecker().checkArguments(call, sym->params, sym->minArgs, sym->hasRest,
                                                   expr->loc(), &given);
  }
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (c->init || !c->constructors.empty()) {
      this->checker.flowChecker().calledFunction();
      break;
    }
  }
  expr->type = cls->type;

  return expr;
}

}
