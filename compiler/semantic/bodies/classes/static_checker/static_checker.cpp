#include "compiler/semantic/bodies/classes/static_checker/static_checker.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/class_member_checker/class_member_checker.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/bodies/classes/method_call_checker/method_call_checker_errors.h"
#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/events/event_target_checker/event_target_checker.h"
#include "compiler/semantic/library/stream/stream_sources/stream_sources.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;

namespace tz {

StaticChecker::StaticChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->classBases = &all.classBases;
  this->callChecker = &all.callChecker;
  this->streamSources = &all.streamSources;
  this->typeRelations = &all.typeRelations;
  this->classDeclarer = &all.classDeclarer;
  this->returnInferrer = &all.returnInferrer;
  this->argumentChecker = &all.argumentChecker;
  this->typeArgInferrer = &all.typeArgInferrer;
  this->overloadResolver = &all.overloadResolver;
  this->classValueChecker = &all.classValueChecker;
  this->classMemberChecker = &all.classMemberChecker;
  this->eventTargetChecker = &all.eventTargetChecker;
}

ClassField *StaticChecker::staticField(ClassDecl *classDecl, llvm::StringRef name,
                                       ClassDecl **owner) const {
  for (ClassDecl *c = classDecl; c != nullptr; c = c->parent) {
    for (ClassField &field : c->staticOwner()->fields) {
      if (field.isStatic() && field.name == name && field.var() != nullptr) {
        *owner = c->staticOwner();
        return &field;
      }
    }
  }

  return nullptr;
}

StaticChecker::ResultStaticFieldType StaticChecker::fieldTyped(const ClassDecl *owner,
                                                               const VarSymbol *variable,
                                                               llvm::StringRef name) const {
  bool isEarly = !owner->declared && this->state.getModuleStates().getLoadingModule() != nullptr &&
                 !this->state.getFunctionStack().isInsideFunction();
  if (variable->type == nullptr && isEarly) {
    auto message = ErrorTextStaticOrder::usedEarly(variable->name, owner->name);

    return err(StaticCheckerFieldTypedError{
        .code = StaticCheckerFieldTypedErrorCode::USED_BEFORE_DECLARATION,
        .message = message,
    });
  }
  if (variable->type == nullptr) {
    auto message = ErrorTextStaticLater::typeOf(variable->name, name);

    return err(StaticCheckerFieldTypedError{
        .code = StaticCheckerFieldTypedErrorCode::TYPE_FROM_LATER_VALUE,
        .message = message,
    });
  }

  return ok(variable->type);
}

StaticChecker::ResultFieldValueIsSet StaticChecker::fieldReady(const ClassDecl *owner,
                                                               const VarSymbol *variable) const {
  bool isEarly = !owner->declared && this->state.getModuleStates().getLoadingModule() != nullptr &&
                 !this->state.getFunctionStack().isInsideFunction() &&
                 this->state.getModuleStates().findDeclModule(owner) ==
                     this->state.getModuleStates().getLoadingModule();
  if (isEarly) {
    auto message = ErrorTextStaticOrder::usedEarly(variable->name, owner->name);

    return err(StaticCheckerFieldReadyError{
        .code = StaticCheckerFieldReadyErrorCode::USED_BEFORE_DECLARATION,
        .message = message,
    });
  }
  bool isUnset = this->state.getClassContext().isInsideStaticInit() &&
                 this->state.getClassContext().getCurrentClass() == owner &&
                 !this->state.getClassContext().isStaticSet(variable);
  if (isUnset) {
    auto message = ErrorTextStaticOrder::beforeSet(variable->name);

    return err(StaticCheckerFieldReadyError{
        .code = StaticCheckerFieldReadyErrorCode::READ_BEFORE_SET,
        .message = message,
    });
  }

  return ok(variable);
}

StaticChecker::ResultStaticFunctions StaticChecker::staticMethods(const ClassDecl *classDecl,
                                                                  llvm::StringRef name) const {
  auto methods = this->classDeclarer->staticMethodsNamed(classDecl, name);
  bool isInstance =
      methods.empty() && (this->classBases->findField(classDecl, name) != nullptr ||
                          !this->classDeclarer->methodsNamed(classDecl, name).empty());
  if (isInstance) {
    auto message = ErrorTextStaticMember::instance(name, classDecl->name);

    return err(StaticCheckerStaticMethodsError{
        .code = StaticCheckerStaticMethodsErrorCode::INSTANCE_MEMBER,
        .message = message,
    });
  }
  if (methods.empty()) {
    auto message = ErrorTextStaticMember::noMember(classDecl->name, name);

    return err(StaticCheckerStaticMethodsError{
        .code = StaticCheckerStaticMethodsErrorCode::NO_STATIC_MEMBER,
        .message = message,
    });
  }

  return ok(methods);
}

Expr *StaticChecker::checkStaticMember(MemberExpr *member, ClassDecl *classDecl,
                                       const Type *expected) {
  member->type = Type::error();
  ClassDecl *owner = nullptr;
  auto *found = this->staticField(classDecl, member->name, &owner);
  if (found != nullptr) {
    return this->checkStaticFieldRead(member, classDecl, *found, owner);
  }

  return this->checkStaticMethodValue(member, classDecl, expected);
}

Expr *StaticChecker::checkStaticFieldRead(MemberExpr *member, ClassDecl *classDecl,
                                          const ClassField &field, const ClassDecl *owner) {
  llvm::StringRef name = member->name;
  auto accessible = this->classMemberChecker->checkAccess(owner, field.access, name);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
  }
  auto *read = this->state.getAst().make<NameExpr>(member->loc(), field.var()->name);
  read->binding = NamedVar{.var = field.var()};
  if (member->object != nullptr && member->object->loc() != member->loc()) {
    this->state.getInventoryStore().rememberNamedUse(read, {classDecl, member->object->loc(), ""});
  }
  auto typed = this->fieldTyped(owner, field.var(), name);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(member->loc(), typed.error().message);
    read->type = Type::error();
    return read;
  }
  auto ready = this->fieldReady(owner, field.var());
  if (ready.isErr()) {
    this->state.getDiagnostics().error(member->loc(), ready.error().message);
  }
  read->type = typed.value();
  if (read->type->isOptional() && this->state.getFlowState().isDefined(field.var())) {
    return this->typeRelations->convert(read, read->type->inner());
  }

  return read;
}

Expr *StaticChecker::checkStaticMethodValue(MemberExpr *member, ClassDecl *classDecl,
                                            const Type *expected) {
  llvm::StringRef name = member->name;
  auto found = this->staticMethods(classDecl, name);
  bool isClassValue = found.isErr() && (name == "name" || name == "prototype");
  if (isClassValue) {
    return this->checkClassValueMember(member, classDecl);
  }
  if (found.isErr()) {
    this->state.getDiagnostics().error(member->loc(), found.error().message);
    return member;
  }
  std::vector<FunctionDecl *> methods = found.value();
  auto accessible =
      this->classMemberChecker->checkAccess(methods[0]->cls(), methods[0]->access(), name);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
  }
  auto variant = this->overloadResolver->variantForType(methods, expected, member);
  if (variant.isErr()) {
    this->state.getDiagnostics().error(member->loc(), variant.error().message);
    return member;
  }
  FuncSymbol *function = variant.value();
  auto *value = this->state.getAst().make<NameExpr>(member->loc(), name);
  value->binding = NamedFunction{.func = function};
  value->type = this->returnInferrer->functionType(function);

  return value;
}

Expr *StaticChecker::checkClassValueMember(MemberExpr *member, ClassDecl *classDecl) {
  auto *object = cast<NameExpr>(member->object);
  member->object = this->classValueChecker->checkClassValue(object, classDecl);
  auto read = this->classValueChecker->checkClassRefMember(member);
  if (read.isErr()) {
    this->state.getDiagnostics().error(member->loc(), read.error().message);
    return member;
  }

  return read.value();
}

StaticChecker::ResultFieldOfTheClass StaticChecker::fieldFound(const ClassField *found,
                                                               const ClassDecl *classDecl,
                                                               llvm::StringRef name) const {
  bool isMethod =
      found == nullptr && !this->classDeclarer->staticMethodsNamed(classDecl, name).empty();
  if (isMethod) {
    auto message = ErrorTextStaticField::isMethod(name, classDecl->name);

    return err(StaticCheckerFieldFoundError{
        .code = StaticCheckerFieldFoundErrorCode::STATIC_METHOD,
        .message = message,
    });
  }
  if (found == nullptr) {
    auto message = ErrorTextStaticField::noField(classDecl->name, name);

    return err(StaticCheckerFieldFoundError{
        .code = StaticCheckerFieldFoundErrorCode::NO_STATIC_FIELD,
        .message = message,
    });
  }

  return ok(found);
}

StaticChecker::ResultFieldIsWritable StaticChecker::fieldWritable(const VarSymbol *variable) const {
  if (variable->isConst) {
    auto message = ErrorTextStaticField::readOnly(variable->name);

    return err(StaticCheckerFieldWritableError{
        .code = StaticCheckerFieldWritableErrorCode::READ_ONLY,
        .message = message,
    });
  }

  return ok(variable);
}

StaticChecker::ResultFieldTypeStated StaticChecker::typeDeclared(const VarSymbol *variable,
                                                                 llvm::StringRef name) const {
  if (variable->type == nullptr) {
    auto message = ErrorTextStaticLater::typeOf(variable->name, name);

    return err(StaticCheckerTypeDeclaredError{
        .code = StaticCheckerTypeDeclaredErrorCode::TYPE_FROM_LATER_VALUE,
        .message = message,
    });
  }

  return ok(variable->type);
}

const Type *StaticChecker::checkStaticTarget(Expr *&target, ClassDecl *classDecl) {
  auto *member = cast<MemberExpr>(target);
  member->object->type = member->type = Type::error();
  ClassDecl *owner = nullptr;
  auto *found = this->staticField(classDecl, member->name, &owner);
  auto field = this->fieldFound(found, classDecl, member->name);
  if (field.isErr()) {
    this->state.getDiagnostics().error(member->loc(), field.error().message);
    return Type::error();
  }
  ClassField &f = *found;
  auto accessible = this->classMemberChecker->checkAccess(owner, f.access, f.name);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
  }
  auto *name = this->state.getAst().make<NameExpr>(member->loc(), f.var()->name);
  name->binding = NamedVar{.var = f.var()};
  name->type = f.var()->type != nullptr ? f.var()->type : Type::error();
  target = name;
  auto writable = this->fieldWritable(f.var());
  if (writable.isErr()) {
    this->state.getDiagnostics().error(member->loc(), writable.error().message);
  }
  f.var()->reassigned = true;
  auto stated = this->typeDeclared(f.var(), f.name);
  if (stated.isErr()) {
    this->state.getDiagnostics().error(member->loc(), stated.error().message);
    return Type::error();
  }

  return stated.value();
}

StaticChecker::ResultFieldIsCallable StaticChecker::staticCallable(const Type *type,
                                                                   const ClassDecl *classDecl,
                                                                   llvm::StringRef name) const {
  if (type->isError()) {
    return err(StaticCheckerStaticCallableError{
        .code = StaticCheckerStaticCallableErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (!type->isFunction()) {
    auto message = ErrorTextStaticCall::notFunc(classDecl->name, name, type->name());

    return err(StaticCheckerStaticCallableError{
        .code = StaticCheckerStaticCallableErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }

  return ok(type);
}

StaticChecker::ResultTypeArgsAllowed StaticChecker::methodTypeArgs(const FuncSymbol *function,
                                                                   const MemberExpr *member) const {
  if (!function->isGeneric() && !member->typeArgs.empty()) {
    auto message = ErrorTextMethodTypeArgs::notGeneric(member->name);

    return err(StaticCheckerMethodTypeArgsError{
        .code = StaticCheckerMethodTypeArgsErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(function);
}

Expr *StaticChecker::checkClassStaticCall(CallExpr *call, MemberExpr *member, ClassDecl *classDecl,
                                          const Type *expected) {
  call->type = Type::error();
  auto *library = this->checkLibraryStaticCall(call, member, classDecl, expected);
  if (library != nullptr) {
    return library;
  }
  auto methods = this->classDeclarer->staticMethodsNamed(classDecl, member->name);
  if (methods.empty()) {
    return this->checkStaticFieldCall(call, member, classDecl);
  }

  return this->checkStaticMethodCall(call, member, methods, expected);
}

Expr *StaticChecker::checkLibraryStaticCall(CallExpr *call, MemberExpr *member,
                                            const ClassDecl *classDecl, const Type *expected) {
  static const llvm::StringLiteral eventsFunctions[] = {
      "once", "on", "listenerCount", "getEventListeners"};
  bool hasEventsFunction = classDecl->library() && classDecl->name == "EventEmitter" &&
                           llvm::is_contained(eventsFunctions, member->name);
  if (hasEventsFunction) {
    auto *events = this->eventTargetChecker->checkEventsFunction(call, member->name, member->loc());
    if (events != nullptr) {
      return events;
    }
    auto *function = this->state.getLibraryDeclarations().findLibraryFunction(member->name);

    return this->callChecker->checkDirectCall(call, function->sym, member->loc(), expected);
  }
  if (classDecl->library() && classDecl->name == "Readable" && member->name == "from") {
    return this->streamSources->checkStreamFrom(call, member);
  }

  return nullptr;
}

Expr *StaticChecker::checkStaticFieldCall(CallExpr *call, MemberExpr *member,
                                          ClassDecl *classDecl) {
  call->callee = this->checkStaticMember(member, classDecl, nullptr);
  auto callable = this->staticCallable(call->callee->type, classDecl, member->name);
  if (callable.isOk()) {
    auto valued = this->callChecker->checkValueCall(call, callable.value());
    if (valued.isErr()) {
      this->state.getDiagnostics().error(call->callee->loc(), valued.error().message);
      return call;
    }
    return valued.value();
  }
  if (callable.error().code != StaticCheckerStaticCallableErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(member->loc(), callable.error().message);
  }
  this->argumentChecker->walkArgsLoosely(call);

  return call;
}

Expr *StaticChecker::checkStaticMethodCall(CallExpr *call, MemberExpr *member,
                                           const std::vector<FunctionDecl *> &methods,
                                           const Type *expected) {
  auto accessible =
      this->classMemberChecker->checkAccess(methods[0]->cls(), methods[0]->access(), member->name);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
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
  auto allowed = this->methodTypeArgs(function, member);
  if (allowed.isErr()) {
    this->state.getDiagnostics().error(member->loc(), allowed.error().message);
  }
  if (function->isGeneric() && isChecked) {
    return this->overloadResolver->callGenericFunction(call, function, bound, member->loc());
  }
  if (function->isGeneric()) {
    return this->typeArgInferrer->checkGenericCall(call, function, member->loc(), expected,
                                                   member->typeArgs);
  }
  call->target = FunctionCall{.func = function};
  call->type = function->getReturnType();
  std::vector<bool> given(isChecked ? call->args.size() : 0, true);

  return this->argumentChecker->checkArguments(call, function->params, function->minArgs,
                                               function->hasRest, member->loc(), &given);
}

}
