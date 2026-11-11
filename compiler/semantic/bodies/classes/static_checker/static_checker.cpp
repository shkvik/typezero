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
  this->callChecker = &all.callChecker;
  this->streamSources = &all.streamSources;
  this->typeRelations = &all.typeRelations;
  this->returnInferrer = &all.returnInferrer;
  this->argumentChecker = &all.argumentChecker;
  this->typeArgInferrer = &all.typeArgInferrer;
  this->overloadResolver = &all.overloadResolver;
  this->classValueChecker = &all.classValueChecker;
  this->classMemberChecker = &all.classMemberChecker;
  this->eventTargetChecker = &all.eventTargetChecker;
}

ClassField *StaticChecker::staticField(ClassDecl *cls, llvm::StringRef name, ClassDecl **owner) {
  for (ClassDecl *c = cls; c; c = c->parent) {
    for (ClassField &f : c->staticOwner()->fields) {
      if (f.isStatic() && f.name == name && f.var()) {
        *owner = c->staticOwner();
        return &f;
      }
    }
  }

  return nullptr;
}

StaticChecker::ResultStaticFieldType StaticChecker::fieldTyped(const ClassDecl *owner,
                                                               const VarSymbol *var,
                                                               llvm::StringRef name) const {
  bool early = !owner->declared && this->state.getModuleStates().getLoadingModule() &&
               !this->state.getFunctionStack().isInsideFunction();
  if (!var->type && early) {
    std::string message = ErrorTextStaticOrder::usedEarly(var->name, owner->name);

    return err(StaticCheckerFieldTypedError{
        .code = StaticCheckerFieldTypedErrorCode::USED_BEFORE_DECLARATION,
        .message = message,
    });
  }
  if (!var->type) {
    std::string message = ErrorTextStaticLater::typeOf(var->name, name);

    return err(StaticCheckerFieldTypedError{
        .code = StaticCheckerFieldTypedErrorCode::TYPE_FROM_LATER_VALUE,
        .message = message,
    });
  }

  return ok(var->type);
}

StaticChecker::ResultFieldValueIsSet StaticChecker::fieldReady(const ClassDecl *owner,
                                                               const VarSymbol *var) const {
  bool early = !owner->declared && this->state.getModuleStates().getLoadingModule() &&
               !this->state.getFunctionStack().isInsideFunction() &&
               this->state.getModuleStates().findDeclModule(owner) ==
                   this->state.getModuleStates().getLoadingModule();
  if (early) {
    std::string message = ErrorTextStaticOrder::usedEarly(var->name, owner->name);

    return err(StaticCheckerFieldReadyError{
        .code = StaticCheckerFieldReadyErrorCode::USED_BEFORE_DECLARATION,
        .message = message,
    });
  }
  bool unset = this->state.getClassContext().isInsideStaticInit() &&
               this->state.getClassContext().getCurrentClass() == owner &&
               !this->state.getClassContext().isStaticSet(var);
  if (unset) {
    std::string message = ErrorTextStaticOrder::beforeSet(var->name);

    return err(StaticCheckerFieldReadyError{
        .code = StaticCheckerFieldReadyErrorCode::READ_BEFORE_SET,
        .message = message,
    });
  }

  return ok(var);
}

StaticChecker::ResultStaticFunctions StaticChecker::staticMethods(const ClassDecl *cls,
                                                                  llvm::StringRef name) {
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, true);
  bool instance = methods.empty() && (ClassBases::findField(cls, name) ||
                                      !ClassDeclarer::methodsNamed(cls, name, false).empty());
  if (instance) {
    std::string message = ErrorTextStaticMember::instance(name, cls->name);

    return err(StaticCheckerStaticMethodsError{
        .code = StaticCheckerStaticMethodsErrorCode::INSTANCE_MEMBER,
        .message = message,
    });
  }
  if (methods.empty()) {
    std::string message = ErrorTextStaticMember::noMember(cls->name, name);

    return err(StaticCheckerStaticMethodsError{
        .code = StaticCheckerStaticMethodsErrorCode::NO_STATIC_MEMBER,
        .message = message,
    });
  }

  return ok(methods);
}

Expr *StaticChecker::checkStaticMember(MemberExpr *member, ClassDecl *cls, const Type *expected) {
  member->type = Type::error();
  ClassDecl *owner = nullptr;
  ClassField *found = StaticChecker::staticField(cls, member->name, &owner);
  if (found != nullptr) {
    return this->checkStaticFieldRead(member, cls, *found, owner);
  }

  return this->checkStaticMethodValue(member, cls, expected);
}

Expr *StaticChecker::checkStaticFieldRead(MemberExpr *member, ClassDecl *cls, const ClassField &f,
                                          const ClassDecl *owner) {
  llvm::StringRef name = member->name;
  ClassMemberChecker::ResultMemberAccessible accessible =
      this->classMemberChecker->checkAccess(owner, f.access, name);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
  }
  auto *read = this->state.getAst().make<NameExpr>(member->loc(), f.var()->name);
  read->binding = NamedVar{.var = f.var()};
  if (member->object != nullptr && member->object->loc() != member->loc()) {
    this->state.getInventoryStore().rememberNamedUse(read, {cls, member->object->loc(), ""});
  }
  ResultStaticFieldType typed = this->fieldTyped(owner, f.var(), name);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(member->loc(), typed.error().message);
    read->type = Type::error();
    return read;
  }
  ResultFieldValueIsSet ready = this->fieldReady(owner, f.var());
  if (ready.isErr()) {
    this->state.getDiagnostics().error(member->loc(), ready.error().message);
  }
  read->type = typed.value();
  if (read->type->isOptional() && this->state.getFlowState().isDefined(f.var())) {
    return this->typeRelations->convert(read, read->type->inner());
  }

  return read;
}

Expr *StaticChecker::checkStaticMethodValue(MemberExpr *member, ClassDecl *cls,
                                            const Type *expected) {
  llvm::StringRef name = member->name;
  ResultStaticFunctions found = StaticChecker::staticMethods(cls, name);
  bool classValue = found.isErr() && (name == "name" || name == "prototype");
  if (classValue) {
    return this->checkClassValueMember(member, cls);
  }
  if (found.isErr()) {
    this->state.getDiagnostics().error(member->loc(), found.error().message);
    return member;
  }
  std::vector<FunctionDecl *> methods = found.value();
  ClassMemberChecker::ResultMemberAccessible accessible =
      this->classMemberChecker->checkAccess(methods[0]->cls(), methods[0]->access(), name);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
  }
  OverloadResolver::ResultVariantOfType variant =
      this->overloadResolver->variantForType(methods, expected, member);
  if (variant.isErr()) {
    this->state.getDiagnostics().error(member->loc(), variant.error().message);
    return member;
  }
  FuncSymbol *sym = variant.value();
  auto *value = this->state.getAst().make<NameExpr>(member->loc(), name);
  value->binding = NamedFunction{.func = sym};
  value->type = this->returnInferrer->functionType(sym);

  return value;
}

Expr *StaticChecker::checkClassValueMember(MemberExpr *member, ClassDecl *cls) {
  auto *object = cast<NameExpr>(member->object);
  member->object = this->classValueChecker->checkClassValue(object, cls);
  ClassValueChecker::ResultClassMemberRead read =
      this->classValueChecker->checkClassRefMember(member);
  if (read.isErr()) {
    this->state.getDiagnostics().error(member->loc(), read.error().message);
    return member;
  }

  return read.value();
}

StaticChecker::ResultFieldOfTheClass
StaticChecker::fieldFound(const ClassField *found, const ClassDecl *cls, llvm::StringRef name) {
  bool method = !found && !ClassDeclarer::methodsNamed(cls, name, true).empty();
  if (method) {
    std::string message = ErrorTextStaticField::isMethod(name, cls->name);

    return err(StaticCheckerFieldFoundError{
        .code = StaticCheckerFieldFoundErrorCode::STATIC_METHOD,
        .message = message,
    });
  }
  if (!found) {
    std::string message = ErrorTextStaticField::noField(cls->name, name);

    return err(StaticCheckerFieldFoundError{
        .code = StaticCheckerFieldFoundErrorCode::NO_STATIC_FIELD,
        .message = message,
    });
  }

  return ok(found);
}

StaticChecker::ResultFieldIsWritable StaticChecker::fieldWritable(const VarSymbol *var) {
  if (var->isConst) {
    std::string message = ErrorTextStaticField::readOnly(var->name);

    return err(StaticCheckerFieldWritableError{
        .code = StaticCheckerFieldWritableErrorCode::READ_ONLY,
        .message = message,
    });
  }

  return ok(var);
}

StaticChecker::ResultFieldTypeStated StaticChecker::typeDeclared(const VarSymbol *var,
                                                                 llvm::StringRef name) {
  if (!var->type) {
    std::string message = ErrorTextStaticLater::typeOf(var->name, name);

    return err(StaticCheckerTypeDeclaredError{
        .code = StaticCheckerTypeDeclaredErrorCode::TYPE_FROM_LATER_VALUE,
        .message = message,
    });
  }

  return ok(var->type);
}

const Type *StaticChecker::checkStaticTarget(Expr *&target, ClassDecl *cls) {
  auto *member = cast<MemberExpr>(target);
  member->object->type = member->type = Type::error();
  ClassDecl *owner = nullptr;
  ClassField *found = StaticChecker::staticField(cls, member->name, &owner);
  ResultFieldOfTheClass field = StaticChecker::fieldFound(found, cls, member->name);
  if (field.isErr()) {
    this->state.getDiagnostics().error(member->loc(), field.error().message);
    return Type::error();
  }
  ClassField &f = *found;
  ClassMemberChecker::ResultMemberAccessible accessible =
      this->classMemberChecker->checkAccess(owner, f.access, f.name);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
  }
  auto *name = this->state.getAst().make<NameExpr>(member->loc(), f.var()->name);
  name->binding = NamedVar{.var = f.var()};
  name->type = f.var()->type ? f.var()->type : Type::error();
  target = name;
  ResultFieldIsWritable writable = StaticChecker::fieldWritable(f.var());
  if (writable.isErr()) {
    this->state.getDiagnostics().error(member->loc(), writable.error().message);
  }
  f.var()->reassigned = true;
  ResultFieldTypeStated stated = StaticChecker::typeDeclared(f.var(), f.name);
  if (stated.isErr()) {
    this->state.getDiagnostics().error(member->loc(), stated.error().message);
    return Type::error();
  }

  return stated.value();
}

StaticChecker::ResultFieldIsCallable
StaticChecker::staticCallable(const Type *type, const ClassDecl *cls, llvm::StringRef name) {
  if (type->isError()) {
    return err(StaticCheckerStaticCallableError{
        .code = StaticCheckerStaticCallableErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (!type->isFunction()) {
    std::string message = ErrorTextStaticCall::notFunc(cls->name, name, type->name());

    return err(StaticCheckerStaticCallableError{
        .code = StaticCheckerStaticCallableErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }

  return ok(type);
}

StaticChecker::ResultTypeArgsAllowed StaticChecker::methodTypeArgs(const FuncSymbol *sym,
                                                                   const MemberExpr *member) {
  if (!sym->isGeneric() && !member->typeArgs.empty()) {
    std::string message = ErrorTextMethodTypeArgs::notGeneric(member->name);

    return err(StaticCheckerMethodTypeArgsError{
        .code = StaticCheckerMethodTypeArgsErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(sym);
}

Expr *StaticChecker::checkClassStaticCall(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                                          const Type *expected) {
  call->type = Type::error();
  Expr *library = this->checkLibraryStaticCall(call, member, cls, expected);
  if (library != nullptr) {
    return library;
  }
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, member->name, true);
  if (methods.empty()) {
    return this->checkStaticFieldCall(call, member, cls);
  }

  return this->checkStaticMethodCall(call, member, methods, expected);
}

Expr *StaticChecker::checkLibraryStaticCall(CallExpr *call, MemberExpr *member,
                                            const ClassDecl *cls, const Type *expected) {
  static const llvm::StringLiteral eventsFunctions[] = {
      "once", "on", "listenerCount", "getEventListeners"};
  bool eventsFunction = cls->library() && cls->name == "EventEmitter" &&
                        llvm::is_contained(eventsFunctions, member->name);
  if (eventsFunction) {
    Expr *events = this->eventTargetChecker->checkEventsFunction(call, member->name, member->loc());
    if (events != nullptr) {
      return events;
    }
    FunctionDecl *function = this->state.getLibraryDeclarations().findLibraryFunction(member->name);

    return this->callChecker->checkDirectCall(call, function->sym, member->loc(), expected);
  }
  if (cls->library() && cls->name == "Readable" && member->name == "from") {
    return this->streamSources->checkStreamFrom(call, member);
  }

  return nullptr;
}

Expr *StaticChecker::checkStaticFieldCall(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  call->callee = this->checkStaticMember(member, cls, nullptr);
  ResultFieldIsCallable callable =
      StaticChecker::staticCallable(call->callee->type, cls, member->name);
  if (callable.isOk()) {
    CallChecker::ResultValueCallTyped valued =
        this->callChecker->checkValueCall(call, callable.value());
    if (valued.isErr()) {
      this->state.getDiagnostics().error(call->callee->loc(), valued.error().message);
      return call;
    }
    return valued.value();
  }
  if (callable.error().code != StaticCheckerStaticCallableErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(member->loc(), callable.error().message);
  }
  this->argumentChecker->checkArgsLoosely(call);

  return call;
}

Expr *StaticChecker::checkStaticMethodCall(CallExpr *call, MemberExpr *member,
                                           const std::vector<FunctionDecl *> &methods,
                                           const Type *expected) {
  ClassMemberChecker::ResultMemberAccessible accessible =
      this->classMemberChecker->checkAccess(methods[0]->cls(), methods[0]->access(), member->name);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
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
  ResultTypeArgsAllowed allowed = StaticChecker::methodTypeArgs(sym, member);
  if (allowed.isErr()) {
    this->state.getDiagnostics().error(member->loc(), allowed.error().message);
  }
  if (sym->isGeneric() && checked) {
    return this->overloadResolver->callGenericVariant(call, sym, bound, member->loc(), false);
  }
  if (sym->isGeneric()) {
    return this->typeArgInferrer->checkGenericCall(call, sym, member->loc(), expected,
                                                   member->typeArgs);
  }
  call->target = FunctionCall{.func = sym};
  call->type = sym->getReturnType();
  std::vector<bool> given(checked ? call->args.size() : 0, true);

  return this->argumentChecker->checkArguments(call, sym->params, sym->minArgs, sym->hasRest,
                                               member->loc(), &given);
}

}
