#include "compiler/semantic/bodies/classes/inherit_checker/inherit_checker.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/classes/class_member_checker/class_member_checker.h"
#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver.h"
#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver_errors.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/semantic/generics/widening_relations/widening_relations.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/events/event_maps/event_maps.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

InheritChecker::InheritChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->eventMaps = &all.eventMaps;
  this->classLinks = &all.classLinks;
  this->classBases = &all.classBases;
  this->flowChecker = &all.flowChecker;
  this->classChecker = &all.classChecker;
  this->classDeclarer = &all.classDeclarer;
  this->emitterChecker = &all.emitterChecker;
  this->returnInferrer = &all.returnInferrer;
  this->argumentChecker = &all.argumentChecker;
  this->typeArgInferrer = &all.typeArgInferrer;
  this->overloadResolver = &all.overloadResolver;
  this->classMemberChecker = &all.classMemberChecker;
}

bool InheritChecker::takesParams(const Type *functionType, const FuncSymbol *function) const {
  return functionType->isFunction() &&
         functionType->params() == llvm::ArrayRef<const Type *>(function->params) &&
         functionType->minArgs() == function->minArgs &&
         functionType->hasRest() == function->hasRest;
}

bool InheritChecker::isArrowMethod(const FunctionDecl *functionDecl) {
  if (functionDecl->cls() == nullptr) {
    return false;
  }
  for (const ClassField &field : functionDecl->cls()->fields) {
    if (field.method() == functionDecl) {
      return true;
    }
  }

  return false;
}

void InheritChecker::interfacesOf(const ClassDecl *classDecl,
                                  std::vector<const Type *> &out) const {
  std::vector<const Type *> queue(classDecl->implemented.begin(), classDecl->implemented.end());
  for (size_t index = 0; index < queue.size(); ++index) {
    if (llvm::is_contained(out, queue[index])) {
      continue;
    }
    out.push_back(queue[index]);
    for (const Type *base : queue[index]->bases()) {
      queue.push_back(base);
    }
  }
}

InheritChecker::ResultAbstractHasClass
InheritChecker::abstractNeedsClass(const ClassDecl *classDecl,
                                   const FunctionDecl *functionDecl) const {
  if (functionDecl->isAbstract() && !classDecl->isAbstract) {
    auto message = ErrorTextAbstract::needsClass(functionDecl->name, classDecl->name);

    return err(InheritCheckerAbstractNeedsClassError{
        .code = InheritCheckerAbstractNeedsClassErrorCode::ABSTRACT_IN_CONCRETE,
        .message = message,
    });
  }

  return ok(functionDecl);
}

InheritChecker::ResultAbstractOverride
InheritChecker::abstractImplemented(const ClassDecl *classDecl, const FunctionDecl *found) const {
  if (found->cls() != classDecl) {
    auto variant = ErrorTextVariant::text(found);
    auto message = ErrorTextAbstract::notImpl(classDecl->name, variant, found->cls()->name);

    return err(InheritCheckerAbstractImplementedError{
        .code = InheritCheckerAbstractImplementedErrorCode::NOT_IMPLEMENTED,
        .message = message,
    });
  }

  return ok(found);
}

void InheritChecker::walkAbstract(ClassDecl *classDecl) {
  if (classDecl->literal() != nullptr) {
    return;
  }
  for (FunctionDecl *functionDecl : this->classLinks->ownMethods(classDecl)) {
    auto abstract = this->abstractNeedsClass(classDecl, functionDecl);
    if (abstract.isErr()) {
      this->state.getDiagnostics().error(functionDecl->loc(), abstract.error().message);
    }
  }
  if (classDecl->isAbstract) {
    return;
  }
  for (size_t slot = 0; slot < classDecl->vtable.size(); ++slot) {
    if (classDecl->vtable[slot] != nullptr) {
      continue;
    }
    for (const ClassDecl *c = classDecl; c != nullptr; c = c->parent) {
      FunctionDecl *found = nullptr;
      for (FunctionDecl *functionDecl : this->classLinks->ownMethods(c)) {
        if (functionDecl->slot() == int(slot)) {
          found = functionDecl;
        }
      }
      if (found == nullptr) {
        continue;
      }
      auto implemented = this->abstractImplemented(classDecl, found);
      if (implemented.isErr()) {
        this->state.getDiagnostics().error(classDecl->loc(), implemented.error().message);
      }
      break;
    }
  }
}

InheritChecker::ResultFieldTypeMatches
InheritChecker::fieldMatches(llvm::StringRef prefix, const Field &member, const Field *actual,
                             const ClassDecl *classDecl) const {
  bool isSame =
      actual->type == member.type || WideningRelations::isSameReference(actual->type, member.type);
  if (!isSame) {
    auto message = ErrorTextImplements::fieldType(prefix, member.name, member.type->name(),
                                                  classDecl->name, actual->type->name());

    return err(InheritCheckerFieldMatchesError{
        .code = InheritCheckerFieldMatchesErrorCode::FIELD_TYPE_DIFFERS,
        .message = message,
    });
  }

  return ok(actual);
}

InheritChecker::ResultMemberIsDeclared
InheritChecker::memberDeclared(llvm::StringRef prefix, const Field &member,
                               llvm::ArrayRef<FunctionDecl *> methods) const {
  if (methods.empty()) {
    bool isOptional = member.type->isOptional();
    std::string type = isOptional ? member.type->inner()->name().str() : member.type->name().str();
    auto message = ErrorTextImplements::noMember(prefix, member.name, isOptional, type);

    return err(InheritCheckerMemberDeclaredError{
        .code = InheritCheckerMemberDeclaredErrorCode::NO_MEMBER,
        .message = message,
    });
  }

  return ok(&member);
}

InheritChecker::ResultMemberIsFunction
InheritChecker::methodForField(const Field &member, const Type *interfaceType,
                               const ClassDecl *classDecl) const {
  if (!member.type->isFunction()) {
    auto message = ErrorTextImplements::fieldNotFn(member.name, member.type->name(),
                                                   interfaceType->name(), classDecl->name);

    return err(InheritCheckerMethodForFieldError{
        .code = InheritCheckerMethodForFieldErrorCode::FIELD_AS_METHOD,
        .message = message,
    });
  }

  return ok(&member);
}

InheritChecker::ResultReplaceableField
InheritChecker::replaceable(const Field &member, llvm::ArrayRef<FunctionDecl *> methods,
                            const Type *interfaceType, const ClassDecl *classDecl) const {
  if (!member.readonly && llvm::none_of(methods, InheritChecker::isArrowMethod)) {
    auto message =
        ErrorTextImplements::mustField(member.name, interfaceType->name(), classDecl->name);

    return err(InheritCheckerReplaceableError{
        .code = InheritCheckerReplaceableErrorCode::METHOD_NOT_FIELD,
        .message = message,
    });
  }

  return ok(&member);
}

InheritChecker::ResultReturnTypeAgrees
InheritChecker::returnMatches(llvm::StringRef name, const Type *returnType, const Type *expected,
                              const Type *interfaceType, const ClassDecl *classDecl) const {
  if (returnType != nullptr && !returnType->isError() && returnType != expected) {
    auto message = ErrorTextImplements::returns(name, returnType->name(), interfaceType->name(),
                                                classDecl->name, expected->name());

    return err(InheritCheckerReturnMatchesError{
        .code = InheritCheckerReturnMatchesErrorCode::RETURN_DIFFERS,
        .message = message,
    });
  }

  return ok(returnType);
}

InheritChecker::ResultVariantForParams
InheritChecker::variantFound(llvm::StringRef prefix, const Field &member,
                             llvm::ArrayRef<FunctionDecl *> methods, const ClassDecl *classDecl,
                             bool found) const {
  if (!found) {
    auto variants = ErrorTextVariant::list(methods);
    auto message = ErrorTextImplements::params(prefix, member.name, member.type->name(),
                                               classDecl->name, variants);

    return err(InheritCheckerVariantFoundError{
        .code = InheritCheckerVariantFoundErrorCode::OTHER_PARAMETERS,
        .message = message,
    });
  }

  return ok(&member);
}

InheritChecker::ResultSignatureVariant InheritChecker::signatureVariant(llvm::StringRef prefix,
                                                                        llvm::StringRef name,
                                                                        const Type *signature,
                                                                        bool found) const {
  if (!found) {
    auto message = ErrorTextImplements::noVariant(prefix, name, signature->name());

    return err(InheritCheckerSignatureVariantError{
        .code = InheritCheckerSignatureVariantErrorCode::NO_VARIANT,
        .message = message,
    });
  }

  return ok(signature);
}

void InheritChecker::walkImplements(ClassDecl *classDecl) {
  auto interfaces = std::vector<const Type *>();
  this->interfacesOf(classDecl, interfaces);
  for (const Type *interfaceType : interfaces) {
    if (!llvm::is_contained(classDecl->implemented, interfaceType)) {
      continue;
    }
    auto prefix = ErrorTextImplements::prefix(classDecl->name, interfaceType->name());
    for (const Field &member : interfaceType->fields()) {
      this->walkImplementedMember(classDecl, interfaceType, member, prefix);
    }
    for (const Overload &overload : interfaceType->overloads()) {
      this->walkImplementedOverload(classDecl, interfaceType, overload, prefix);
    }
  }
}

void InheritChecker::walkImplementedMember(ClassDecl *classDecl, const Type *interfaceType,
                                           const Field &member, const std::string &prefix) {
  const auto *field = this->classBases->findField(classDecl, member.name);
  if (field != nullptr && field->method() == nullptr) {
    return this->walkImplementedField(classDecl, member, field, prefix);
  }
  auto methods = this->classDeclarer->methodsNamed(classDecl, member.name);
  auto declared = this->memberDeclared(prefix, member, methods);
  if (declared.isErr()) {
    this->state.getDiagnostics().error(classDecl->loc(), declared.error().message);
    return;
  }
  auto function = this->methodForField(member, interfaceType, classDecl);
  if (function.isErr()) {
    this->state.getDiagnostics().error(methods[0]->loc(), function.error().message);
    return;
  }
  auto replaced = this->replaceable(member, methods, interfaceType, classDecl);
  if (replaced.isErr()) {
    this->state.getDiagnostics().error(methods[0]->loc(), replaced.error().message);
    return;
  }
  bool isFound = this->hasMatchingVariant(classDecl, interfaceType, member, methods);
  auto variant = this->variantFound(prefix, member, methods, classDecl, isFound);
  if (variant.isErr()) {
    this->state.getDiagnostics().error(methods[0]->loc(), variant.error().message);
  }
}

void InheritChecker::walkImplementedField(const ClassDecl *classDecl, const Field &member,
                                          const ClassField *field, const std::string &prefix) {
  const auto *actual = classDecl->type->field(member.name);
  if (actual == nullptr || actual->type->isError() || member.type->isError()) {
    return;
  }
  llvm::SMLoc loc = classDecl->field(member.name) != nullptr ? field->loc : classDecl->loc();
  auto matches = this->fieldMatches(prefix, member, actual, classDecl);
  if (matches.isErr()) {
    this->state.getDiagnostics().error(loc, matches.error().message);
  }
}

bool InheritChecker::hasMatchingVariant(const ClassDecl *classDecl, const Type *interfaceType,
                                        const Field &member,
                                        llvm::ArrayRef<FunctionDecl *> methods) {
  bool isFound = false;
  for (FunctionDecl *functionDecl : methods) {
    if (functionDecl->sym == nullptr || !this->takesParams(member.type, functionDecl->sym)) {
      continue;
    }
    isFound = true;
    const auto *returnType = functionDecl->sym->getReturnType();
    auto agrees = this->returnMatches(functionDecl->name, returnType, member.type->ret(),
                                      interfaceType, classDecl);
    if (agrees.isErr()) {
      this->state.getDiagnostics().error(functionDecl->loc(), agrees.error().message);
    }
  }

  return isFound;
}

void InheritChecker::walkImplementedOverload(ClassDecl *classDecl, const Type *interfaceType,
                                             const Overload &overload, const std::string &prefix) {
  auto methods = this->classDeclarer->methodsNamed(classDecl, overload.name);
  for (const Type *signature : overload.signatures) {
    auto *functionDecl = this->methodTaking(methods, signature);
    auto variant =
        this->signatureVariant(prefix, overload.name, signature, functionDecl != nullptr);
    if (variant.isErr()) {
      this->state.getDiagnostics().error(classDecl->loc(), variant.error().message);
      continue;
    }
    const auto *returnType = functionDecl->sym->getReturnType();
    auto agrees =
        this->returnMatches(overload.name, returnType, signature->ret(), interfaceType, classDecl);
    if (agrees.isErr()) {
      this->state.getDiagnostics().error(functionDecl->loc(), agrees.error().message);
    }
  }
}

FunctionDecl *InheritChecker::methodTaking(llvm::ArrayRef<FunctionDecl *> methods,
                                           const Type *signature) const {
  for (FunctionDecl *method : methods) {
    if (method->sym != nullptr && this->takesParams(signature, method->sym)) {
      return method;
    }
  }

  return nullptr;
}

InheritChecker::ResultConstructorScope
InheritChecker::superAllowed(FunctionDecl *constructor) const {
  if (constructor == nullptr || !constructor->isConstructor() || constructor->cls() == nullptr) {
    auto message = ErrorTextSuperCall::outside();

    return err(InheritCheckerSuperAllowedError{
        .code = InheritCheckerSuperAllowedErrorCode::NOT_IN_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok(constructor);
}

InheritChecker::ResultParentClassFound InheritChecker::parentClass(ClassDecl *classDecl) const {
  if (classDecl->parent == nullptr && !classDecl->base) {
    auto message = ErrorTextSuperCall::noParent(classDecl->name);

    return err(InheritCheckerParentClassError{
        .code = InheritCheckerParentClassErrorCode::NO_PARENT,
        .message = message,
    });
  }
  if (classDecl->parent == nullptr) {
    return err(InheritCheckerParentClassError{
        .code = InheritCheckerParentClassErrorCode::BASE_ONLY,
        .message = "",
    });
  }

  return ok(classDecl->parent);
}

InheritChecker::ResultSuperIsStatement InheritChecker::rootSuper(const CallExpr *call) const {
  if (!this->state.getClassStore().isRootSuper(call)) {
    auto message = ErrorTextSuperCall::statement();

    return err(InheritCheckerRootSuperError{
        .code = InheritCheckerRootSuperErrorCode::NOT_A_STATEMENT,
        .message = message,
    });
  }

  return ok(call);
}

InheritChecker::ResultSuperNotRepeated InheritChecker::superOnce(const ClassDecl *classDecl) const {
  if (!this->state.getFlowState().isBeforeSuper()) {
    auto message = ErrorTextSuperCall::twice(classDecl->parent->name);

    return err(InheritCheckerSuperOnceError{
        .code = InheritCheckerSuperOnceErrorCode::CALLED_TWICE,
        .message = message,
    });
  }

  return ok(classDecl);
}

InheritChecker::ResultNoSuperArguments
InheritChecker::superArguments(const CallExpr *call, const ClassDecl *parent) const {
  if (!call->args.empty()) {
    auto message = ErrorTextSuperCall::noArgs(parent->name);

    return err(InheritCheckerSuperArgumentsError{
        .code = InheritCheckerSuperArgumentsErrorCode::NO_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok(call);
}

Expr *InheritChecker::checkSuperCall(CallExpr *call) {
  call->type = Type::voidTy();
  call->callee->type = Type::voidTy();
  call->target = SuperCall{};
  auto *classDecl = this->superCallClass(call);
  if (classDecl == nullptr) {
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  auto constructors = this->classLinks->constructorsOf(classDecl->parent);
  if (constructors.empty()) {
    auto none = this->superArguments(call, classDecl->parent);
    if (none.isErr()) {
      this->state.getDiagnostics().error(call->loc(), none.error().message);
    }
    this->argumentChecker->walkArgsLoosely(call);
  } else {
    this->walkParentConstructor(call, constructors);
  }
  this->state.getFlowState().markSuperCalled();
  for (const ClassField &field : classDecl->fields) {
    if (!field.isStatic() && field.init != nullptr && field.method() == nullptr) {
      this->state.getFlowState().addSetField(field.name);
    }
  }
  this->flowChecker->calledFunction();

  return call;
}

ClassDecl *InheritChecker::superCallClass(const CallExpr *call) {
  auto scope = this->superAllowed(this->state.getFunctionStack().getCurrentDecl());
  if (scope.isErr()) {
    this->state.getDiagnostics().error(call->loc(), scope.error().message);
    return nullptr;
  }
  ClassDecl *classDecl = scope.value()->cls();
  auto parent = this->parentClass(classDecl);
  if (parent.isErr()) {
    if (parent.error().code != InheritCheckerParentClassErrorCode::BASE_ONLY) {
      this->state.getDiagnostics().error(call->loc(), parent.error().message);
    }
    return nullptr;
  }
  auto statement = this->rootSuper(call);
  if (statement.isErr()) {
    this->state.getDiagnostics().error(call->loc(), statement.error().message);
    return nullptr;
  }
  auto once = this->superOnce(classDecl);
  if (once.isErr()) {
    this->state.getDiagnostics().error(call->loc(), once.error().message);
    return nullptr;
  }

  return classDecl;
}

void InheritChecker::walkParentConstructor(CallExpr *call,
                                           llvm::ArrayRef<FunctionDecl *> constructors) {
  auto picked = this->overloadResolver->choosePlainVariant(constructors, call->args, call->spread);
  if (picked.isErr() &&
      picked.error().code != OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(call->loc(), picked.error().message);
  }
  if (picked.isErr()) {
    return;
  }
  FuncSymbol *function = picked.value().function;
  bool isChecked = picked.value().isChecked;
  const FunctionDecl *chosen = function->decl != nullptr ? function->decl : constructors[0];
  auto accessible =
      this->classMemberChecker->checkAccess(chosen->cls(), chosen->access(), "constructor");
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(call->loc(), accessible.error().message);
  }
  call->target = SuperCall{.method = function};
  std::vector<bool> given(isChecked ? call->args.size() : 0, true);
  this->argumentChecker->checkArguments(call, function->params, function->minArgs,
                                        function->hasRest, call->loc(), &given);
}

InheritChecker::ResultSuperMethodScope InheritChecker::superContext(FunctionDecl *method) const {
  if (method == nullptr || (method->cls()->parent == nullptr && !method->cls()->base)) {
    auto message = ErrorTextSuperMethod::outside();

    return err(InheritCheckerSuperContextError{
        .code = InheritCheckerSuperContextErrorCode::NO_SUPER_CLASS,
        .message = message,
    });
  }
  if (method->cls()->parent == nullptr) {
    return err(InheritCheckerSuperContextError{
        .code = InheritCheckerSuperContextErrorCode::BASE_ONLY,
        .message = "",
    });
  }

  return ok(method);
}

InheritChecker::ResultObjectIsComplete
InheritChecker::objectComplete(const MemberExpr *member) const {
  bool isEarly = this->classChecker->isInitThis(member->object).isOk() &&
                 !this->state.getFlowState().isBeforeSuper() &&
                 this->classChecker->thisReady(this->state.getFlowState().getFlow()).isErr();
  if (isEarly) {
    std::string field =
        this->classChecker->firstUnset(this->state.getFlowState().getFlow()).value();
    auto message = ErrorTextSuperMethod::early(member->name, field);

    return err(InheritCheckerObjectCompleteError{
        .code = InheritCheckerObjectCompleteErrorCode::CALLED_BEFORE_SET,
        .message = message,
    });
  }

  return ok(member);
}

InheritChecker::ResultParentHasMethods InheritChecker::parentMethods(const ClassDecl *parent,
                                                                     llvm::StringRef name) const {
  auto methods = this->classDeclarer->methodsNamed(parent, name);
  if (methods.empty() && this->classBases->findField(parent, name) != nullptr) {
    auto message = ErrorTextSuperMethod::isField(name, parent->name);

    return err(InheritCheckerParentMethodsError{
        .code = InheritCheckerParentMethodsErrorCode::FIELD_OF_PARENT,
        .message = message,
    });
  }
  if (methods.empty()) {
    auto message = ErrorTextSuperMethod::noMethod(parent->name, name);

    return err(InheritCheckerParentMethodsError{
        .code = InheritCheckerParentMethodsErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  return ok(methods);
}

InheritChecker::ResultMethodHasItsBody InheritChecker::methodBody(FuncSymbol *function,
                                                                  llvm::StringRef name) const {
  if (function->decl->isAbstract()) {
    auto message = ErrorTextSuperMethod::abstract(name, function->decl->cls()->name);

    return err(InheritCheckerMethodBodyError{
        .code = InheritCheckerMethodBodyErrorCode::ABSTRACT_METHOD,
        .message = message,
    });
  }

  return ok(function);
}

Expr *InheritChecker::checkSuperMethodCall(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  member->type = Type::error();
  auto *method = this->enclosingMethod();
  auto scope = this->superContext(method);
  if (scope.isErr()) {
    if (scope.error().code != InheritCheckerSuperContextErrorCode::BASE_ONLY) {
      this->state.getDiagnostics().error(member->object->loc(), scope.error().message);
    }
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  ClassDecl *parent = method->cls()->parent;
  if (!this->superObjectReady(member)) {
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  auto complete = this->objectComplete(member);
  if (complete.isErr()) {
    this->state.getDiagnostics().error(member->loc(), complete.error().message);
  }
  auto found = this->parentMethods(parent, member->name);
  if (found.isErr()) {
    this->state.getDiagnostics().error(member->loc(), found.error().message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  std::vector<FunctionDecl *> methods = found.value();
  auto accessible =
      this->classMemberChecker->checkAccess(methods[0]->cls(), methods[0]->access(), member->name);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
  }
  Expr *emitter = this->eventMaps->isEmitterClass(parent)
                      ? this->emitterChecker->checkEmitterCall(call, member, parent, true)
                      : nullptr;
  if (emitter != nullptr) {
    return emitter;
  }

  return this->callSuperMethod(call, member, methods);
}

FunctionDecl *InheritChecker::enclosingMethod() {
  auto functions = this->state.getFunctionStack().getFunctions();
  for (FunctionDecl *functionDecl : llvm::reverse(functions)) {
    if (functionDecl->isArrow()) {
      continue;
    }
    if (functionDecl->takesThis() && functionDecl->cls()->literal() == nullptr) {
      return functionDecl;
    }
    return nullptr;
  }

  return nullptr;
}

bool InheritChecker::superObjectReady(MemberExpr *member) {
  auto *self = this->state.getAst().make<NameExpr>(member->object->loc(), "this");
  this->state.getClassContext().rememberThisMember(member->name);
  auto reference = this->classChecker->checkThis(self);
  this->state.getClassContext().forgetThisMember();
  if (reference.isErr()) {
    this->state.getDiagnostics().error(self->loc(), reference.error().message);
  }
  member->object = reference.isOk() ? reference.value() : self;

  return !member->object->type->isError();
}

Expr *InheritChecker::callSuperMethod(CallExpr *call, MemberExpr *member,
                                      llvm::ArrayRef<FunctionDecl *> methods) {
  auto picked = this->overloadResolver->chooseVariant(methods, call->args, call->spread);
  if (picked.isErr() &&
      picked.error().code != OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(member->loc(), picked.error().message);
  }
  if (picked.isErr()) {
    return call;
  }
  bool isChecked = picked.value().isChecked;
  const std::vector<const Type *> &bound = picked.value().typeArgs;
  auto body = this->methodBody(picked.value().function, member->name);
  if (body.isErr()) {
    this->state.getDiagnostics().error(member->loc(), body.error().message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  FuncSymbol *function = body.value();
  call->target = SuperCall{};
  if (function->isGeneric() && isChecked) {
    return this->overloadResolver->callGenericMethod(call, function, bound, member->loc());
  }
  if (function->isGeneric()) {
    return this->checkGenericSuperCall(call, member, function);
  }
  call->target = SuperCall{.method = function};
  member->type = this->returnInferrer->functionType(function);
  call->type = function->getReturnType();
  std::vector<bool> given(isChecked ? call->args.size() : 0, true);

  return this->argumentChecker->checkArguments(call, function->params, function->minArgs,
                                               function->hasRest, member->loc(), &given);
}

Expr *InheritChecker::checkGenericSuperCall(CallExpr *call, MemberExpr *member,
                                            FuncSymbol *function) {
  this->typeArgInferrer->checkGenericCall(call, function, member->loc(), nullptr, member->typeArgs);
  call->target = SuperCall{.method = call->func()};
  if (call->method() != nullptr) {
    member->type = this->returnInferrer->functionType(call->method());
  }

  return call;
}

}
