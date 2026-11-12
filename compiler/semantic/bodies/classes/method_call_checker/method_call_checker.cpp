#include "compiler/semantic/bodies/classes/method_call_checker/method_call_checker.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
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
  this->classLinks = &all.classLinks;
  this->classBases = &all.classBases;
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

MethodCallChecker::ResultFieldIsCallable
MethodCallChecker::checkFieldCallable(const Type *type, llvm::StringRef name) const {
  bool isOptional = type->isOptional() && type->inner()->isFunction();
  if (isOptional) {
    auto message = ErrorTextFieldCall::undefined();

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
    auto message = ErrorTextFieldCall::notFunc(name, type->name());

    return err(MethodCallCheckerFieldCallableError{
        .code = MethodCallCheckerFieldCallableErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }

  return ok(type);
}

MethodCallChecker::ResultMethodsAreFound
MethodCallChecker::checkInstanceMethods(const ClassDecl *classDecl, llvm::StringRef name) const {
  auto methods = this->classDeclarer->methodsNamed(classDecl, name);
  bool isStatic =
      methods.empty() && !this->classDeclarer->staticMethodsNamed(classDecl, name).empty();
  if (isStatic) {
    auto message = ErrorTextMethodLookup::isStatic(name, classDecl->name);

    return err(MethodCallCheckerInstanceMethodsError{
        .code = MethodCallCheckerInstanceMethodsErrorCode::STATIC_METHOD,
        .message = message,
    });
  }
  if (methods.empty()) {
    auto message = ErrorTextMethodLookup::noProperty(name, classDecl->type->name());

    return err(MethodCallCheckerInstanceMethodsError{
        .code = MethodCallCheckerInstanceMethodsErrorCode::NO_SUCH_PROPERTY,
        .message = message,
    });
  }

  return ok(methods);
}

MethodCallChecker::ResultFieldsAreAllSet
MethodCallChecker::checkFieldsSet(const MemberExpr *member) const {
  const Flow &flow = this->state.getFlowState().getFlow();
  bool isEarly = this->classChecker->isInitThis(member->object).isOk() &&
                 !this->state.getFlowState().isBeforeSuper() &&
                 this->classChecker->checkThisReady(flow).isErr();
  if (isEarly) {
    auto unset = this->classChecker->firstUnset(flow);
    auto message = ErrorTextUnsetCall::beforeSet(member->name, unset.value());

    return err(MethodCallCheckerFieldsSetError{
        .code = MethodCallCheckerFieldsSetErrorCode::CALLED_BEFORE_SET,
        .message = message,
    });
  }

  return ok(member);
}

MethodCallChecker::ResultTypeArgsAllowed
MethodCallChecker::checkMethodTypeArgs(const FuncSymbol *function, const MemberExpr *member) const {
  if (!function->isGeneric() && !member->typeArgs.empty()) {
    auto message = ErrorTextMethodTypeArgs::notGeneric(member->name);

    return err(MethodCallCheckerMethodTypeArgsError{
        .code = MethodCallCheckerMethodTypeArgsErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(function);
}

Expr *MethodCallChecker::checkMethodCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  auto *classDecl = this->classDeclarer->classOf(member->object->type);
  this->classDeclarer->completeClass(classDecl);
  this->tzModulesChecker->noteTlsMethod(member, classDecl);
  call->type = Type::error();
  member->type = Type::error();
  auto *library = this->checkLibraryMethodCall(call, member, classDecl);
  if (library != nullptr) {
    return library;
  }
  const auto *field = this->classBases->findField(classDecl, member->name);
  if (field != nullptr && field->method() == nullptr) {
    return this->checkFieldCall(call, member);
  }
  auto found = this->checkInstanceMethods(classDecl, member->name);
  if (found.isErr()) {
    this->state.getDiagnostics().error(member->loc(), found.error().message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  std::vector<FunctionDecl *> methods = found.value();
  if (methods.size() == 1) {
    this->walkMethodAccess(member, methods[0]);
  }
  auto ready = this->checkFieldsSet(member);
  if (ready.isErr()) {
    this->state.getDiagnostics().error(member->loc(), ready.error().message);
  }
  auto picked = this->overloadResolver->chooseVariant(methods, call->args, call->spread);
  if (picked.isErr() &&
      picked.error().code != OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(member->loc(), picked.error().message);
  }
  if (picked.isErr()) {
    return call;
  }
  FuncSymbol *function = picked.value().function;
  bool isChecked = picked.value().isChecked;
  const std::vector<const Type *> &bound = picked.value().typeArgs;
  if (methods.size() > 1 && function->decl != nullptr) {
    this->walkMethodAccess(member, function->decl);
  }
  auto allowed = this->checkMethodTypeArgs(function, member);
  if (allowed.isErr()) {
    this->state.getDiagnostics().error(member->loc(), allowed.error().message);
  }
  if (function->isGeneric() && isChecked) {
    return this->overloadResolver->callGenericMethod(call, function, bound, member->loc());
  }
  if (function->isGeneric()) {
    return this->checkGenericMethodCall(call, member, function, expected);
  }

  return this->checkPlainMethodCall(call, member, classDecl, function, isChecked);
}

Expr *MethodCallChecker::checkLibraryMethodCall(CallExpr *call, MemberExpr *member,
                                                ClassDecl *classDecl) {
  auto *fileSystem = this->fsDeclarer->checkFsClassMethod(call, member, classDecl);
  if (fileSystem != nullptr) {
    return fileSystem;
  }
  auto *stream = this->streamChecker->checkStreamCall(call, member, classDecl);
  if (stream != nullptr) {
    return stream;
  }
  Expr *emitter =
      this->eventMaps->isEmitterClass(classDecl)
          ? this->emitterChecker->checkEmitterCall(call, member, classDecl, CallReceiver::OWN)
          : nullptr;
  if (emitter != nullptr) {
    return emitter;
  }
  auto targetName = LibraryClassTable::getName(LibraryClass::EVENT_TARGET);
  auto *target = this->state.getLibraryDeclarations().findLibraryClass(targetName);
  if (target == nullptr || !classDecl->derivesFrom(target)) {
    return nullptr;
  }

  return this->eventTargetChecker->checkEventTargetCall(call, member, classDecl);
}

Expr *MethodCallChecker::checkFieldCall(CallExpr *call, MemberExpr *member) {
  call->callee = this->classMemberChecker->checkClassMember(member, nullptr);
  auto callable = this->checkFieldCallable(call->callee->type, member->name);
  if (callable.isOk()) {
    return this->checkFieldValueCall(call, callable.value());
  }
  if (callable.error().code != MethodCallCheckerFieldCallableErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(member->loc(), callable.error().message);
  }
  this->argumentChecker->walkArgsLoosely(call);

  return call;
}

Expr *MethodCallChecker::checkFieldValueCall(CallExpr *call, const Type *type) {
  auto valued = this->callChecker->checkValueCall(call, type);
  if (valued.isErr()) {
    this->state.getDiagnostics().error(call->callee->loc(), valued.error().message);
    return call;
  }

  return valued.value();
}

void MethodCallChecker::walkMethodAccess(MemberExpr *member, FunctionDecl *method) {
  auto accessible = this->classMemberChecker->checkAccess(method->cls(), method->access(),
                                                          member->name, member->object);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
  }
}

Expr *MethodCallChecker::checkGenericMethodCall(CallExpr *call, MemberExpr *member,
                                                FuncSymbol *function, const Type *expected) {
  this->typeArgInferrer->checkGenericCall(call, function, member->loc(), expected,
                                          member->typeArgs);
  call->target = MethodCall{.method = call->func()};
  if (call->method() != nullptr) {
    member->type = this->returnInferrer->functionType(call->method());
  }

  return call;
}

Expr *MethodCallChecker::checkPlainMethodCall(CallExpr *call, MemberExpr *member,
                                              ClassDecl *classDecl, FuncSymbol *function,
                                              bool isChecked) {
  call->target = this->methodTarget(classDecl, function);
  member->type = this->returnInferrer->functionType(function);
  call->type = function->getReturnType();
  std::vector<bool> given(isChecked ? call->args.size() : 0, true);

  return this->argumentChecker->checkArguments(call, function->params, function->minArgs,
                                               function->hasRest, member->loc(), &given);
}

MethodCall MethodCallChecker::methodTarget(ClassDecl *classDecl, FuncSymbol *function) const {
  auto *implementation = this->classLinks->soleImplementation(classDecl, function->decl);
  if (implementation != nullptr) {
    return MethodCall{.method = implementation->sym};
  }

  return MethodCall{.method = function, .virtualCall = true};
}

MethodCallChecker::ResultClassNotRuntime
MethodCallChecker::checkNotRuntime(const ClassDecl *classDecl) const {
  if (this->fsDeclarer->isRuntimeClass(classDecl)) {
    auto message = ErrorTextRuntimeNew::onlyFs(classDecl->name);

    return err(MethodCallCheckerNotRuntimeError{
        .code = MethodCallCheckerNotRuntimeErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }

  return ok(classDecl);
}

MethodCallChecker::ResultNoTypeArguments
MethodCallChecker::checkClassTypeArgs(const NewExpr *expr, const ClassDecl *classDecl) const {
  if (!expr->typeArgs.empty()) {
    auto message = ErrorTextClassTypeArgs::notGeneric(classDecl->name);

    return err(MethodCallCheckerClassTypeArgsError{
        .code = MethodCallCheckerClassTypeArgsErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(expr);
}

MethodCallChecker::ResultClassIsDeclared
MethodCallChecker::checkClassDeclared(const ClassDecl *classDecl) const {
  bool isEarly = !classDecl->staticOwner()->declared &&
                 this->state.getModuleStates().getLoadingModule() != nullptr &&
                 !this->state.getFunctionStack().isInsideFunction() &&
                 this->state.getModuleStates().findDeclModule(classDecl) ==
                     this->state.getModuleStates().getLoadingModule();
  if (isEarly) {
    auto message = ErrorTextClassOrder::usedEarly(classDecl->name);

    return err(MethodCallCheckerClassDeclaredError{
        .code = MethodCallCheckerClassDeclaredErrorCode::USED_BEFORE_DECLARATION,
        .message = message,
    });
  }

  return ok(classDecl);
}

MethodCallChecker::ResultClassIsConcrete
MethodCallChecker::checkClassConcrete(const ClassDecl *classDecl) const {
  if (classDecl->isAbstract) {
    auto message = ErrorTextAbstractNew::abstract(classDecl->name);

    return err(MethodCallCheckerClassConcreteError{
        .code = MethodCallCheckerClassConcreteErrorCode::ABSTRACT_CLASS,
        .message = message,
    });
  }

  return ok(classDecl);
}

MethodCallChecker::ResultArgumentsAbsent
MethodCallChecker::checkArgumentsAbsent(const CallExpr *call, const ClassDecl *classDecl) const {
  if (!call->args.empty()) {
    auto message = ErrorTextNoConstructor::noArgs(classDecl->name);

    return err(MethodCallCheckerArgumentsAbsentError{
        .code = MethodCallCheckerArgumentsAbsentErrorCode::NO_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok(call);
}

Expr *MethodCallChecker::checkClassNew(NewExpr *expr, ClassDecl *classDecl, const Type *expected) {
  expr->type = Type::error();
  if (classDecl->isGeneric()) {
    return this->genericClassChecker->checkGenericNew(expr, classDecl, expected);
  }
  auto possible = this->checkNotRuntime(classDecl);
  if (possible.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), possible.error().message);
    return this->failClassNew(expr);
  }
  this->walkNewClass(expr, classDecl);
  this->classDeclarer->completeClass(classDecl);
  auto *call = this->state.getAst().make<CallExpr>(expr->loc(), nullptr, std::move(expr->args));
  expr->form = ClassNew{.cls = classDecl, .ctor = call};
  call->type = Type::voidTy();
  auto constructors = this->classLinks->constructorsOf(classDecl);
  bool isMade = constructors.empty() ? !this->reportUnexpectedArgs(expr, call, classDecl)
                                     : this->chooseConstructor(expr, call, constructors);
  if (!isMade) {
    return expr;
  }
  if (this->hasInitializer(classDecl)) {
    this->flowChecker->calledFunction();
  }
  expr->type = classDecl->type;

  return expr;
}

Expr *MethodCallChecker::failClassNew(NewExpr *expr) {
  for (Expr *&arg : expr->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return expr;
}

void MethodCallChecker::walkNewClass(const NewExpr *expr, const ClassDecl *classDecl) {
  auto plain = this->checkClassTypeArgs(expr, classDecl);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), plain.error().message);
  }
  auto declared = this->checkClassDeclared(classDecl);
  if (declared.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), declared.error().message);
  }
  auto concrete = this->checkClassConcrete(classDecl);
  if (concrete.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), concrete.error().message);
  }
}

bool MethodCallChecker::reportUnexpectedArgs(const NewExpr *expr, CallExpr *call,
                                             const ClassDecl *classDecl) {
  auto absent = this->checkArgumentsAbsent(call, classDecl);
  if (absent.isOk()) {
    return false;
  }
  this->state.getDiagnostics().error(expr->loc(), absent.error().message);
  this->argumentChecker->walkArgsLoosely(call);

  return true;
}

bool MethodCallChecker::chooseConstructor(const NewExpr *expr, CallExpr *call,
                                          llvm::ArrayRef<FunctionDecl *> constructors) {
  auto picked = this->overloadResolver->choosePlainVariant(constructors, call->args, {});
  if (picked.isErr() &&
      picked.error().code != OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(expr->loc(), picked.error().message);
  }
  if (picked.isErr()) {
    return false;
  }
  FuncSymbol *function = picked.value().function;
  bool isChecked = picked.value().isChecked;
  const FunctionDecl *chosen = function->decl != nullptr ? function->decl : constructors[0];
  auto constructorName = ObjectMemberTable::getName(ObjectMember::CONSTRUCTOR);
  auto accessible =
      this->classMemberChecker->checkAccess(chosen->cls(), chosen->access(), constructorName);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), accessible.error().message);
  }
  call->target = FunctionCall{.func = function};
  std::vector<bool> given(isChecked ? call->args.size() : 0, true);
  this->argumentChecker->checkArguments(call, function->params, function->minArgs,
                                        function->hasRest, expr->loc(), &given);

  return true;
}

bool MethodCallChecker::hasInitializer(const ClassDecl *classDecl) const {
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    if (current->init != nullptr || !current->constructors.empty()) {
      return true;
    }
  }

  return false;
}

}
