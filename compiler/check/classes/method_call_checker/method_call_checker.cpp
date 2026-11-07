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

MethodCallChecker::ResultFieldIsCallable MethodCallChecker::fieldCallable(const Type *type,
                                                                          llvm::StringRef name) {
  bool optional = type->isOptional() && type->inner()->isFunction();
  if (optional) {
    std::string message = ErrorTextFieldCall::undefined();

    return err(MethodCallCheckerFieldCallableError{
        .code = MethodCallCheckerFieldCallableErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type->isError()) {
    return err(MethodCallCheckerFieldCallableError{
        .code = MethodCallCheckerFieldCallableErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (!type->isFunction()) {
    std::string message = ErrorTextFieldCall::notFunc(name, type->name());

    return err(MethodCallCheckerFieldCallableError{
        .code = MethodCallCheckerFieldCallableErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }

  return ok(type);
}

MethodCallChecker::ResultMethodsAreFound MethodCallChecker::instanceMethods(const ClassDecl *cls,
                                                                            llvm::StringRef name) {
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, false);
  bool isStatic = methods.empty() && !ClassDeclarer::methodsNamed(cls, name, true).empty();
  if (isStatic) {
    std::string message = ErrorTextMethodLookup::isStatic(name, cls->name);

    return err(MethodCallCheckerInstanceMethodsError{
        .code = MethodCallCheckerInstanceMethodsErrorCode::STATIC_METHOD,
        .message = message,
    });
  }
  if (methods.empty()) {
    std::string message = ErrorTextMethodLookup::noProperty(name, cls->type->name());

    return err(MethodCallCheckerInstanceMethodsError{
        .code = MethodCallCheckerInstanceMethodsErrorCode::NO_SUCH_PROPERTY,
        .message = message,
    });
  }

  return ok(methods);
}

MethodCallChecker::ResultFieldsAreAllSet
MethodCallChecker::fieldsSet(const MemberExpr *member) const {
  bool early = this->checker.classChecker().isInitThis(member->object).isOk() &&
               !this->checker.flow_.beforeSuper &&
               this->checker.classChecker().thisReady(this->checker.flow_).isErr();
  if (early) {
    std::string field = this->checker.classChecker().firstUnset(this->checker.flow_).value();
    std::string message = ErrorTextUnsetCall::beforeSet(member->name, field);

    return err(MethodCallCheckerFieldsSetError{
        .code = MethodCallCheckerFieldsSetErrorCode::CALLED_BEFORE_SET,
        .message = message,
    });
  }

  return ok(member);
}

MethodCallChecker::ResultTypeArgsAllowed
MethodCallChecker::methodTypeArgs(const FuncSymbol *sym, const MemberExpr *member) {
  if (!sym->isGeneric() && !member->typeArgs.empty()) {
    std::string message = ErrorTextMethodTypeArgs::notGeneric(member->name);

    return err(MethodCallCheckerMethodTypeArgsError{
        .code = MethodCallCheckerMethodTypeArgsErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(sym);
}

Expr *MethodCallChecker::checkMethodCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  ClassDecl *cls = ClassDeclarer::classOf(member->object->type);
  this->checker.classDeclarer().completeClass(cls);
  this->checker.tzModulesChecker().noteTlsMethod(member, cls);
  call->type = Type::error();
  member->type = Type::error();
  llvm::StringRef name = member->name;
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
    ResultFieldIsCallable callable = MethodCallChecker::fieldCallable(call->callee->type, name);
    if (callable.isOk()) {
      CallChecker::ResultValueCallTyped valued =
          this->checker.callChecker().checkValueCall(call, callable.value());
      if (valued.isErr()) {
        this->checker.diags_.error(call->callee->loc(), valued.error().message);
        return call;
      }
      return valued.value();
    }
    if (callable.error().code != MethodCallCheckerFieldCallableErrorCode::ALREADY_REPORTED) {
      this->checker.diags_.error(member->loc(), callable.error().message);
    }
    this->checker.argumentChecker().checkArgsLoosely(call);
    return call;
  }
  ResultMethodsAreFound found = MethodCallChecker::instanceMethods(cls, name);
  if (found.isErr()) {
    this->checker.diags_.error(member->loc(), found.error().message);
    this->checker.argumentChecker().checkArgsLoosely(call);
    return call;
  }
  std::vector<FunctionDecl *> methods = found.value();
  if (methods.size() == 1) {
    ClassMemberChecker::ResultMemberAccessible accessible =
        this->checker.classMemberChecker().checkAccess(methods[0]->cls(), methods[0]->access(),
                                                       name, member->object);
    if (accessible.isErr()) {
      this->checker.diags_.error(member->loc(), accessible.error().message);
    }
  }
  ResultFieldsAreAllSet ready = this->fieldsSet(member);
  if (ready.isErr()) {
    this->checker.diags_.error(member->loc(), ready.error().message);
  }
  bool checked = false;
  std::vector<const Type *> bound;
  OverloadResolver::ResultVariantChosen picked = this->checker.overloadResolver().chooseVariant(
      methods, call->args, call->spread, checked, &bound);
  if (picked.isErr() &&
      picked.error().code != OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED) {
    this->checker.diags_.error(member->loc(), picked.error().message);
  }
  if (picked.isErr()) {
    return call;
  }
  FuncSymbol *sym = picked.value();
  if (methods.size() > 1 && sym->decl) {
    ClassMemberChecker::ResultMemberAccessible accessible =
        this->checker.classMemberChecker().checkAccess(sym->decl->cls(), sym->decl->access(), name,
                                                       member->object);
    if (accessible.isErr()) {
      this->checker.diags_.error(member->loc(), accessible.error().message);
    }
  }
  this->checker.returnInferrer().resolveParams(sym);
  ResultTypeArgsAllowed allowed = MethodCallChecker::methodTypeArgs(sym, member);
  if (allowed.isErr()) {
    this->checker.diags_.error(member->loc(), allowed.error().message);
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

MethodCallChecker::ResultClassNotRuntime MethodCallChecker::notRuntime(const ClassDecl *cls) const {
  if (this->checker.fsDeclarer().isRuntimeClass(cls)) {
    std::string message = ErrorTextRuntimeNew::onlyFs(cls->name);

    return err(MethodCallCheckerNotRuntimeError{
        .code = MethodCallCheckerNotRuntimeErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }

  return ok(cls);
}

MethodCallChecker::ResultNoTypeArguments MethodCallChecker::classTypeArgs(const NewExpr *expr,
                                                                          const ClassDecl *cls) {
  if (!expr->typeArgs.empty()) {
    std::string message = ErrorTextClassTypeArgs::notGeneric(cls->name);

    return err(MethodCallCheckerClassTypeArgsError{
        .code = MethodCallCheckerClassTypeArgsErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(expr);
}

MethodCallChecker::ResultClassIsDeclared
MethodCallChecker::classDeclared(const ClassDecl *cls) const {
  bool early = !cls->staticOwner()->declared && this->checker.loading_ &&
               this->checker.functions_.empty() &&
               this->checker.declModules_.lookup(cls) == this->checker.loading_;
  if (early) {
    std::string message = ErrorTextClassOrder::usedEarly(cls->name);

    return err(MethodCallCheckerClassDeclaredError{
        .code = MethodCallCheckerClassDeclaredErrorCode::USED_BEFORE_DECLARATION,
        .message = message,
    });
  }

  return ok(cls);
}

MethodCallChecker::ResultClassIsConcrete MethodCallChecker::classConcrete(const ClassDecl *cls) {
  if (cls->isAbstract) {
    std::string message = ErrorTextAbstractNew::abstract(cls->name);

    return err(MethodCallCheckerClassConcreteError{
        .code = MethodCallCheckerClassConcreteErrorCode::ABSTRACT_CLASS,
        .message = message,
    });
  }

  return ok(cls);
}

MethodCallChecker::ResultArgumentsAbsent MethodCallChecker::argumentsAbsent(const CallExpr *call,
                                                                            const ClassDecl *cls) {
  if (!call->args.empty()) {
    std::string message = ErrorTextNoConstructor::noArgs(cls->name);

    return err(MethodCallCheckerArgumentsAbsentError{
        .code = MethodCallCheckerArgumentsAbsentErrorCode::NO_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok(call);
}

Expr *MethodCallChecker::checkClassNew(NewExpr *expr, ClassDecl *cls, const Type *expected) {
  expr->type = Type::error();
  if (cls->isGeneric()) {
    return this->checker.genericClassChecker().checkGenericNew(expr, cls, expected);
  }
  ResultClassNotRuntime possible = this->notRuntime(cls);
  if (possible.isErr()) {
    this->checker.diags_.error(expr->loc(), possible.error().message);
    for (Expr *&arg : expr->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
    return expr;
  }
  ResultNoTypeArguments plain = MethodCallChecker::classTypeArgs(expr, cls);
  if (plain.isErr()) {
    this->checker.diags_.error(expr->loc(), plain.error().message);
  }
  ResultClassIsDeclared declared = this->classDeclared(cls);
  if (declared.isErr()) {
    this->checker.diags_.error(expr->loc(), declared.error().message);
  }
  ResultClassIsConcrete concrete = MethodCallChecker::classConcrete(cls);
  if (concrete.isErr()) {
    this->checker.diags_.error(expr->loc(), concrete.error().message);
  }
  this->checker.classDeclarer().completeClass(cls);
  auto *call = this->checker.ctx_.make<CallExpr>(expr->loc(), nullptr, std::move(expr->args));
  expr->form = ClassNew{.cls = cls, .ctor = call};
  call->type = Type::voidTy();
  std::vector<FunctionDecl *> ctors = ClassLinks::constructorsOf(cls);
  if (ctors.empty()) {
    ResultArgumentsAbsent absent = MethodCallChecker::argumentsAbsent(call, cls);
    if (absent.isErr()) {
      this->checker.diags_.error(expr->loc(), absent.error().message);
      this->checker.argumentChecker().checkArgsLoosely(call);
      return expr;
    }
  } else {
    bool checked = false;
    OverloadResolver::ResultVariantChosen picked =
        this->checker.overloadResolver().chooseVariant(ctors, call->args, {}, checked);
    if (picked.isErr() &&
        picked.error().code != OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED) {
      this->checker.diags_.error(expr->loc(), picked.error().message);
    }
    if (picked.isErr()) {
      return expr;
    }
    FuncSymbol *sym = picked.value();
    const FunctionDecl *chosen = sym->decl ? sym->decl : ctors[0];
    ClassMemberChecker::ResultMemberAccessible accessible =
        this->checker.classMemberChecker().checkAccess(chosen->cls(), chosen->access(),
                                                       "constructor");
    if (accessible.isErr()) {
      this->checker.diags_.error(expr->loc(), accessible.error().message);
    }
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
