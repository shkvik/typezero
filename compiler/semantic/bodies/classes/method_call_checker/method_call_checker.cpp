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
  this->eventMaps = &all.eventMaps;
  this->fsDeclarer = &all.fsDeclarer;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->classChecker = &all.classChecker;
  this->classDeclarer = &all.classDeclarer;
  this->streamChecker = &all.streamChecker;
  this->emitterChecker = &all.emitterChecker;
  this->returnInferrer = &all.returnInferrer;
  this->argumentChecker = &all.argumentChecker;
  this->typeArgInferrer = &all.typeArgInferrer;
  this->overloadResolver = &all.overloadResolver;
  this->tzModulesChecker = &all.tzModulesChecker;
  this->classMemberChecker = &all.classMemberChecker;
  this->eventTargetChecker = &all.eventTargetChecker;
  this->genericClassChecker = &all.genericClassChecker;
}

MethodCallChecker::ResultFieldIsCallable MethodCallChecker::fieldCallable(const Type *type,
                                                                          llvm::StringRef name) {
  bool isOptional = type->isOptional() && type->inner()->isFunction();
  if (isOptional) {
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
  bool isEarly = this->classChecker->isInitThis(member->object).isOk() &&
                 !this->state.getFlowState().isBeforeSuper() &&
                 this->classChecker->thisReady(this->state.getFlowState().getFlow()).isErr();
  if (isEarly) {
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
  Expr *library = this->checkLibraryMethodCall(call, member, cls);
  if (library != nullptr) {
    return library;
  }
  const ClassField *field = ClassBases::findField(cls, member->name);
  if (field != nullptr && !field->method()) {
    return this->checkFieldCall(call, member);
  }
  ResultMethodsAreFound found = MethodCallChecker::instanceMethods(cls, member->name);
  if (found.isErr()) {
    this->state.getDiagnostics().error(member->loc(), found.error().message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  std::vector<FunctionDecl *> methods = found.value();
  if (methods.size() == 1) {
    this->walkMethodAccess(member, methods[0]);
  }
  ResultFieldsAreAllSet ready = this->fieldsSet(member);
  if (ready.isErr()) {
    this->state.getDiagnostics().error(member->loc(), ready.error().message);
  }
  bool isChecked = false;
  std::vector<const Type *> bound;
  OverloadResolver::ResultVariantChosen picked =
      this->overloadResolver->chooseVariant(methods, call->args, call->spread, isChecked, &bound);
  if (picked.isErr() &&
      picked.error().code != OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(member->loc(), picked.error().message);
  }
  if (picked.isErr()) {
    return call;
  }
  FuncSymbol *sym = picked.value();
  if (methods.size() > 1 && sym->decl != nullptr) {
    this->walkMethodAccess(member, sym->decl);
  }
  ResultTypeArgsAllowed allowed = MethodCallChecker::methodTypeArgs(sym, member);
  if (allowed.isErr()) {
    this->state.getDiagnostics().error(member->loc(), allowed.error().message);
  }
  if (sym->isGeneric() && isChecked) {
    return this->overloadResolver->callGenericVariant(call, sym, bound, member->loc(), true);
  }
  if (sym->isGeneric()) {
    return this->checkGenericMethodCall(call, member, sym, expected);
  }

  return this->checkPlainMethodCall(call, member, cls, sym, isChecked);
}

Expr *MethodCallChecker::checkLibraryMethodCall(CallExpr *call, MemberExpr *member,
                                                ClassDecl *cls) {
  Expr *fs = this->fsDeclarer->checkFsClassMethod(call, member, cls);
  if (fs != nullptr) {
    return fs;
  }
  Expr *stream = this->streamChecker->checkStreamCall(call, member, cls);
  if (stream != nullptr) {
    return stream;
  }
  Expr *emitter = this->eventMaps->isEmitterClass(cls)
                      ? this->emitterChecker->checkEmitterCall(call, member, cls, false)
                      : nullptr;
  if (emitter != nullptr) {
    return emitter;
  }
  ClassDecl *target = this->state.getLibraryDeclarations().findLibraryClass("EventTarget");
  if (target == nullptr || !cls->derivesFrom(target)) {
    return nullptr;
  }

  return this->eventTargetChecker->checkEventTargetCall(call, member, cls);
}

Expr *MethodCallChecker::checkFieldCall(CallExpr *call, MemberExpr *member) {
  call->callee = this->classMemberChecker->checkClassMember(member, nullptr);
  ResultFieldIsCallable callable =
      MethodCallChecker::fieldCallable(call->callee->type, member->name);
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
  this->argumentChecker->walkArgsLoosely(call);

  return call;
}

void MethodCallChecker::walkMethodAccess(MemberExpr *member, FunctionDecl *method) {
  ClassMemberChecker::ResultMemberAccessible accessible = this->classMemberChecker->checkAccess(
      method->cls(), method->access(), member->name, member->object);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
  }
}

Expr *MethodCallChecker::checkGenericMethodCall(CallExpr *call, MemberExpr *member, FuncSymbol *sym,
                                                const Type *expected) {
  this->typeArgInferrer->checkGenericCall(call, sym, member->loc(), expected, member->typeArgs);
  call->target = MethodCall{.method = call->func()};
  if (call->method() != nullptr) {
    member->type = this->returnInferrer->functionType(call->method());
  }

  return call;
}

Expr *MethodCallChecker::checkPlainMethodCall(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                                              FuncSymbol *sym, bool checked) {
  FunctionDecl *impl = ClassLinks::soleImplementation(cls, sym->decl);
  if (impl != nullptr) {
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
  bool isEarly = !cls->staticOwner()->declared &&
                 this->state.getModuleStates().getLoadingModule() &&
                 !this->state.getFunctionStack().isInsideFunction() &&
                 this->state.getModuleStates().findDeclModule(cls) ==
                     this->state.getModuleStates().getLoadingModule();
  if (isEarly) {
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
  this->walkNewClass(expr, cls);
  this->classDeclarer->completeClass(cls);
  auto *call = this->state.getAst().make<CallExpr>(expr->loc(), nullptr, std::move(expr->args));
  expr->form = ClassNew{.cls = cls, .ctor = call};
  call->type = Type::voidTy();
  std::vector<FunctionDecl *> ctors = ClassLinks::constructorsOf(cls);
  bool isMade = ctors.empty() ? this->noArgumentsFit(expr, call, cls)
                              : this->constructorChosen(expr, call, ctors);
  if (!isMade) {
    return expr;
  }
  if (MethodCallChecker::hasInitializer(cls)) {
    this->flowChecker->calledFunction();
  }
  expr->type = cls->type;

  return expr;
}

void MethodCallChecker::walkNewClass(const NewExpr *expr, const ClassDecl *cls) {
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
}

bool MethodCallChecker::noArgumentsFit(const NewExpr *expr, CallExpr *call, const ClassDecl *cls) {
  ResultArgumentsAbsent absent = MethodCallChecker::argumentsAbsent(call, cls);
  if (absent.isOk()) {
    return true;
  }
  this->state.getDiagnostics().error(expr->loc(), absent.error().message);
  this->argumentChecker->walkArgsLoosely(call);

  return false;
}

bool MethodCallChecker::constructorChosen(const NewExpr *expr, CallExpr *call,
                                          llvm::ArrayRef<FunctionDecl *> ctors) {
  bool isChecked = false;
  OverloadResolver::ResultVariantChosen picked =
      this->overloadResolver->chooseVariant(ctors, call->args, {}, isChecked);
  if (picked.isErr() &&
      picked.error().code != OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(expr->loc(), picked.error().message);
  }
  if (picked.isErr()) {
    return false;
  }
  FuncSymbol *sym = picked.value();
  const FunctionDecl *chosen = sym->decl != nullptr ? sym->decl : ctors[0];
  ClassMemberChecker::ResultMemberAccessible accessible =
      this->classMemberChecker->checkAccess(chosen->cls(), chosen->access(), "constructor");
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), accessible.error().message);
  }
  call->target = FunctionCall{.func = sym};
  std::vector<bool> given(isChecked ? call->args.size() : 0, true);
  this->argumentChecker->checkArguments(call, sym->params, sym->minArgs, sym->hasRest, expr->loc(),
                                        &given);

  return true;
}

bool MethodCallChecker::hasInitializer(const ClassDecl *cls) {
  for (const ClassDecl *c = cls; c != nullptr; c = c->parent) {
    if (c->init != nullptr || !c->constructors.empty()) {
      return true;
    }
  }

  return false;
}

}
