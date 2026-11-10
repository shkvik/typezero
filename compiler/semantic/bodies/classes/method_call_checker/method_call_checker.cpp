#include "compiler/semantic/bodies/classes/method_call_checker/method_call_checker.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/classes/class_member_checker/class_member_checker.h"
#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/semantic/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/events/event_maps/event_maps.h"
#include "compiler/semantic/library/events/event_target_checker/event_target_checker.h"
#include "compiler/semantic/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/semantic/library/stream/stream_checker/stream_checker.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

MethodCallChecker::MethodCallChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->eventMaps = &all.getEventMaps();
  this->fsDeclarer = &all.getFsDeclarer();
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
  this->flowChecker = &all.getFlowChecker();
  this->classChecker = &all.getClassChecker();
  this->classDeclarer = &all.getClassDeclarer();
  this->streamChecker = &all.getStreamChecker();
  this->emitterChecker = &all.getEmitterChecker();
  this->returnInferrer = &all.getReturnInferrer();
  this->argumentChecker = &all.getArgumentChecker();
  this->typeArgInferrer = &all.getTypeArgInferrer();
  this->overloadResolver = &all.getOverloadResolver();
  this->tzModulesChecker = &all.getTzModulesChecker();
  this->classMemberChecker = &all.getClassMemberChecker();
  this->eventTargetChecker = &all.getEventTargetChecker();
  this->genericClassChecker = &all.getGenericClassChecker();
}

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
  bool early = this->classChecker->isInitThis(member->object).isOk() &&
               !this->state.getFlowState().isBeforeSuper() &&
               this->classChecker->thisReady(this->state.getFlowState().getFlow()).isErr();
  if (early) {
    std::string field =
        this->classChecker->firstUnset(this->state.getFlowState().getFlow()).value();
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
  this->classDeclarer->completeClass(cls);
  this->tzModulesChecker->noteTlsMethod(member, cls);
  call->type = Type::error();
  member->type = Type::error();
  llvm::StringRef name = member->name;
  if (Expr *checked = this->fsDeclarer->checkFsClassMethod(call, member, cls)) {
    return checked;
  }
  if (Expr *checked = this->streamChecker->checkStreamCall(call, member, cls)) {
    return checked;
  }
  if (this->eventMaps->isEmitterClass(cls)) {
    if (Expr *checked = this->emitterChecker->checkEmitterCall(call, member, cls, false)) {
      return checked;
    }
  }
  if (ClassDecl *target = this->state.getLibraryDeclarations().findLibraryClass("EventTarget");
      target && cls->derivesFrom(target)) {
    if (Expr *checked = this->eventTargetChecker->checkEventTargetCall(call, member, cls)) {
      return checked;
    }
  }
  const ClassField *field = ClassBases::findField(cls, name);
  if (field && !field->method()) {
    call->callee = this->classMemberChecker->checkClassMember(member, nullptr);
    ResultFieldIsCallable callable = MethodCallChecker::fieldCallable(call->callee->type, name);
    if (callable.isOk()) {
      CallChecker::ResultValueCallTyped valued =
          this->callChecker->checkValueCall(call, callable.value());
      if (valued.isErr()) {
        this->state.getDiagnostics().error(call->callee->loc(), valued.error().message);
        return call;
      }
      return valued.value();
    }
    if (callable.error().code != MethodCallCheckerFieldCallableErrorCode::ALREADY_REPORTED) {
      this->state.getDiagnostics().error(member->loc(), callable.error().message);
    }
    this->argumentChecker->checkArgsLoosely(call);
    return call;
  }
  ResultMethodsAreFound found = MethodCallChecker::instanceMethods(cls, name);
  if (found.isErr()) {
    this->state.getDiagnostics().error(member->loc(), found.error().message);
    this->argumentChecker->checkArgsLoosely(call);
    return call;
  }
  std::vector<FunctionDecl *> methods = found.value();
  if (methods.size() == 1) {
    ClassMemberChecker::ResultMemberAccessible accessible = this->classMemberChecker->checkAccess(
        methods[0]->cls(), methods[0]->access(), name, member->object);
    if (accessible.isErr()) {
      this->state.getDiagnostics().error(member->loc(), accessible.error().message);
    }
  }
  ResultFieldsAreAllSet ready = this->fieldsSet(member);
  if (ready.isErr()) {
    this->state.getDiagnostics().error(member->loc(), ready.error().message);
  }
  bool checked = false;
  std::vector<const Type *> bound;
  OverloadResolver::ResultVariantChosen picked =
      this->overloadResolver->chooseVariant(methods, call->args, call->spread, checked, &bound);
  if (picked.isErr() &&
      picked.error().code != OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(member->loc(), picked.error().message);
  }
  if (picked.isErr()) {
    return call;
  }
  FuncSymbol *sym = picked.value();
  if (methods.size() > 1 && sym->decl) {
    ClassMemberChecker::ResultMemberAccessible accessible = this->classMemberChecker->checkAccess(
        sym->decl->cls(), sym->decl->access(), name, member->object);
    if (accessible.isErr()) {
      this->state.getDiagnostics().error(member->loc(), accessible.error().message);
    }
  }
  ResultTypeArgsAllowed allowed = MethodCallChecker::methodTypeArgs(sym, member);
  if (allowed.isErr()) {
    this->state.getDiagnostics().error(member->loc(), allowed.error().message);
  }
  if (sym->isGeneric() && checked) {
    return this->overloadResolver->callGenericVariant(call, sym, bound, member->loc(), true);
  }
  if (sym->isGeneric()) {
    this->typeArgInferrer->checkGenericCall(call, sym, member->loc(), expected, member->typeArgs);
    call->target = MethodCall{.method = call->func()};
    if (call->method()) {
      member->type = this->returnInferrer->functionType(call->method());
    }
    return call;
  }
  if (FunctionDecl *impl = ClassLinks::soleImplementation(cls, sym->decl)) {
    call->target = MethodCall{.method = impl->sym};
  } else {
    call->target = MethodCall{.method = sym, .virtualCall = true};
  }
  member->type = this->returnInferrer->functionType(sym);
  call->type = sym->getReturnType();
  std::vector<bool> given(checked ? call->args.size() : 0, true);

  return this->argumentChecker->checkArguments(call, sym->params, sym->minArgs, sym->hasRest,
                                               member->loc(), &given);
}

MethodCallChecker::ResultClassNotRuntime MethodCallChecker::notRuntime(const ClassDecl *cls) const {
  if (this->fsDeclarer->isRuntimeClass(cls)) {
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
  bool early = !cls->staticOwner()->declared && this->state.getModuleStates().getLoadingModule() &&
               !this->state.getFunctionStack().isInsideFunction() &&
               this->state.getModuleStates().findDeclModule(cls) ==
                   this->state.getModuleStates().getLoadingModule();
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
    return this->genericClassChecker->checkGenericNew(expr, cls, expected);
  }
  ResultClassNotRuntime possible = this->notRuntime(cls);
  if (possible.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), possible.error().message);
    for (Expr *&arg : expr->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return expr;
  }
  ResultNoTypeArguments plain = MethodCallChecker::classTypeArgs(expr, cls);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), plain.error().message);
  }
  ResultClassIsDeclared declared = this->classDeclared(cls);
  if (declared.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), declared.error().message);
  }
  ResultClassIsConcrete concrete = MethodCallChecker::classConcrete(cls);
  if (concrete.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), concrete.error().message);
  }
  this->classDeclarer->completeClass(cls);
  auto *call = this->state.getAst().make<CallExpr>(expr->loc(), nullptr, std::move(expr->args));
  expr->form = ClassNew{.cls = cls, .ctor = call};
  call->type = Type::voidTy();
  std::vector<FunctionDecl *> ctors = ClassLinks::constructorsOf(cls);
  if (ctors.empty()) {
    ResultArgumentsAbsent absent = MethodCallChecker::argumentsAbsent(call, cls);
    if (absent.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), absent.error().message);
      this->argumentChecker->checkArgsLoosely(call);
      return expr;
    }
  } else {
    bool checked = false;
    OverloadResolver::ResultVariantChosen picked =
        this->overloadResolver->chooseVariant(ctors, call->args, {}, checked);
    if (picked.isErr() &&
        picked.error().code != OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED) {
      this->state.getDiagnostics().error(expr->loc(), picked.error().message);
    }
    if (picked.isErr()) {
      return expr;
    }
    FuncSymbol *sym = picked.value();
    const FunctionDecl *chosen = sym->decl ? sym->decl : ctors[0];
    ClassMemberChecker::ResultMemberAccessible accessible =
        this->classMemberChecker->checkAccess(chosen->cls(), chosen->access(), "constructor");
    if (accessible.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), accessible.error().message);
    }
    call->target = FunctionCall{.func = sym};
    std::vector<bool> given(checked ? call->args.size() : 0, true);
    this->argumentChecker->checkArguments(call, sym->params, sym->minArgs, sym->hasRest,
                                          expr->loc(), &given);
  }
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (c->init || !c->constructors.empty()) {
      this->flowChecker->calledFunction();
      break;
    }
  }
  expr->type = cls->type;

  return expr;
}

}
