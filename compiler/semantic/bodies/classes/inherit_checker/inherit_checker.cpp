#include "compiler/semantic/bodies/classes/inherit_checker/inherit_checker.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
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

InheritChecker::ResultAbstractHasClass
InheritChecker::checkAbstractNeedsClass(const ClassDecl *classDecl,
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
InheritChecker::checkAbstractImplemented(const ClassDecl *classDecl,
                                         const FunctionDecl *found) const {
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
    auto abstract = this->checkAbstractNeedsClass(classDecl, functionDecl);
    if (abstract.isErr()) {
      this->state.getDiagnostics().error(functionDecl->loc(), abstract.error().message);
    }
  }
  if (classDecl->isAbstract) {
    return;
  }
  for (size_t slot = 0; slot < classDecl->vtable.size(); ++slot) {
    if (classDecl->vtable[slot] == nullptr) {
      this->walkEmptySlot(classDecl, slot);
    }
  }
}

void InheritChecker::walkEmptySlot(const ClassDecl *classDecl, size_t slot) {
  auto *found = this->slotMethod(classDecl, slot);
  if (found == nullptr) {
    return;
  }
  auto implemented = this->checkAbstractImplemented(classDecl, found);
  if (implemented.isErr()) {
    this->state.getDiagnostics().error(classDecl->loc(), implemented.error().message);
  }
}

FunctionDecl *InheritChecker::slotMethod(const ClassDecl *classDecl, size_t slot) const {
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    auto *found = this->ownSlotMethod(current, slot);
    if (found != nullptr) {
      return found;
    }
  }

  return nullptr;
}

FunctionDecl *InheritChecker::ownSlotMethod(const ClassDecl *owner, size_t slot) const {
  auto methods = this->classLinks->ownMethods(owner);
  for (FunctionDecl *functionDecl : llvm::reverse(methods)) {
    if (functionDecl->slot() == int(slot)) {
      return functionDecl;
    }
  }

  return nullptr;
}

InheritChecker::ResultFieldTypeMatches
InheritChecker::checkFieldMatches(llvm::StringRef prefix, const Field &member, const Field *actual,
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
InheritChecker::checkMemberDeclared(llvm::StringRef prefix, const Field &member,
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
InheritChecker::checkMethodForField(const Field &member, const Type *interfaceType,
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
InheritChecker::checkReplaceable(const Field &member, llvm::ArrayRef<FunctionDecl *> methods,
                                 const Type *interfaceType, const ClassDecl *classDecl) const {
  auto isArrow = std::bind_front(&ClassLinks::isArrowMethod, this->classLinks);
  if (!member.readonly && llvm::none_of(methods, isArrow)) {
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
InheritChecker::checkReturnMatches(llvm::StringRef name, const Type *returnType,
                                   const Type *expected, const Type *interfaceType,
                                   const ClassDecl *classDecl) const {
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
InheritChecker::checkVariantFound(llvm::StringRef prefix, const Field &member,
                                  llvm::ArrayRef<FunctionDecl *> methods,
                                  const ClassDecl *classDecl, bool isFound) const {
  if (!isFound) {
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

InheritChecker::ResultSignatureVariant InheritChecker::checkSignatureVariant(llvm::StringRef prefix,
                                                                             llvm::StringRef name,
                                                                             const Type *signature,
                                                                             bool isFound) const {
  if (!isFound) {
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
  this->classLinks->interfacesOf(classDecl, interfaces);
  for (const Type *interfaceType : interfaces) {
    if (llvm::is_contained(classDecl->implemented, interfaceType)) {
      this->walkImplementedInterface(classDecl, interfaceType);
    }
  }
}

void InheritChecker::walkImplementedInterface(ClassDecl *classDecl, const Type *interfaceType) {
  auto prefix = ErrorTextImplements::prefix(classDecl->name, interfaceType->name());
  for (const Field &member : interfaceType->fields()) {
    this->walkImplementedMember(classDecl, interfaceType, member, prefix);
  }
  for (const Overload &overload : interfaceType->overloads()) {
    this->walkImplementedOverload(classDecl, interfaceType, overload, prefix);
  }
}

void InheritChecker::walkImplementedMember(ClassDecl *classDecl, const Type *interfaceType,
                                           const Field &member, const std::string &prefix) {
  const auto *field = this->classBases->findField(classDecl, member.name);
  if (field != nullptr && field->method() == nullptr) {
    return this->walkImplementedField(classDecl, member, field, prefix);
  }
  auto methods = this->classDeclarer->methodsNamed(classDecl, member.name);
  auto declared = this->checkMemberDeclared(prefix, member, methods);
  if (declared.isErr()) {
    this->state.getDiagnostics().error(classDecl->loc(), declared.error().message);
    return;
  }
  auto function = this->checkMethodForField(member, interfaceType, classDecl);
  if (function.isErr()) {
    this->state.getDiagnostics().error(methods[0]->loc(), function.error().message);
    return;
  }
  auto replaced = this->checkReplaceable(member, methods, interfaceType, classDecl);
  if (replaced.isErr()) {
    this->state.getDiagnostics().error(methods[0]->loc(), replaced.error().message);
    return;
  }
  this->walkVariantReturns(classDecl, interfaceType, member, methods);
  bool isFound = this->hasMatchingVariant(member, methods);
  auto variant = this->checkVariantFound(prefix, member, methods, classDecl, isFound);
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
  auto matches = this->checkFieldMatches(prefix, member, actual, classDecl);
  if (matches.isErr()) {
    this->state.getDiagnostics().error(loc, matches.error().message);
  }
}

void InheritChecker::walkVariantReturns(const ClassDecl *classDecl, const Type *interfaceType,
                                        const Field &member,
                                        llvm::ArrayRef<FunctionDecl *> methods) {
  for (FunctionDecl *functionDecl : methods) {
    if (this->isTakingMember(member, functionDecl)) {
      this->walkVariantReturn(classDecl, interfaceType, member, functionDecl);
    }
  }
}

void InheritChecker::walkVariantReturn(const ClassDecl *classDecl, const Type *interfaceType,
                                       const Field &member, const FunctionDecl *functionDecl) {
  const auto *returnType = functionDecl->sym->getReturnType();
  auto agrees = this->checkReturnMatches(functionDecl->name, returnType, member.type->ret(),
                                         interfaceType, classDecl);
  if (agrees.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), agrees.error().message);
  }
}

bool InheritChecker::hasMatchingVariant(const Field &member,
                                        llvm::ArrayRef<FunctionDecl *> methods) const {
  for (const FunctionDecl *functionDecl : methods) {
    if (this->isTakingMember(member, functionDecl)) {
      return true;
    }
  }

  return false;
}

bool InheritChecker::isTakingMember(const Field &member, const FunctionDecl *functionDecl) const {
  return functionDecl->sym != nullptr &&
         this->classLinks->isTakingParams(member.type, functionDecl->sym);
}

void InheritChecker::walkImplementedOverload(ClassDecl *classDecl, const Type *interfaceType,
                                             const Overload &overload, const std::string &prefix) {
  auto methods = this->classDeclarer->methodsNamed(classDecl, overload.name);
  for (const Type *signature : overload.signatures) {
    this->walkOverloadSignature(classDecl, interfaceType, overload, methods, signature, prefix);
  }
}

void InheritChecker::walkOverloadSignature(ClassDecl *classDecl, const Type *interfaceType,
                                           const Overload &overload,
                                           llvm::ArrayRef<FunctionDecl *> methods,
                                           const Type *signature, const std::string &prefix) {
  auto *functionDecl = this->methodTaking(methods, signature);
  auto variant =
      this->checkSignatureVariant(prefix, overload.name, signature, functionDecl != nullptr);
  if (variant.isErr()) {
    this->state.getDiagnostics().error(classDecl->loc(), variant.error().message);
    return;
  }
  const auto *returnType = functionDecl->sym->getReturnType();
  auto agrees = this->checkReturnMatches(overload.name, returnType, signature->ret(), interfaceType,
                                         classDecl);
  if (agrees.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), agrees.error().message);
  }
}

FunctionDecl *InheritChecker::methodTaking(llvm::ArrayRef<FunctionDecl *> methods,
                                           const Type *signature) const {
  for (FunctionDecl *method : methods) {
    if (method->sym != nullptr && this->classLinks->isTakingParams(signature, method->sym)) {
      return method;
    }
  }

  return nullptr;
}

InheritChecker::ResultConstructorScope
InheritChecker::checkSuperAllowed(FunctionDecl *constructor) const {
  if (constructor == nullptr || !constructor->isConstructor() || constructor->cls() == nullptr) {
    auto message = ErrorTextSuperCall::outside();

    return err(InheritCheckerSuperAllowedError{
        .code = InheritCheckerSuperAllowedErrorCode::NOT_IN_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok(constructor);
}

InheritChecker::ResultParentClassFound
InheritChecker::checkParentClass(ClassDecl *classDecl) const {
  if (classDecl->parent == nullptr && !classDecl->base.has_value()) {
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

InheritChecker::ResultSuperIsStatement InheritChecker::checkRootSuper(const CallExpr *call) const {
  if (!this->state.getClassStore().isRootSuper(call)) {
    auto message = ErrorTextSuperCall::statement();

    return err(InheritCheckerRootSuperError{
        .code = InheritCheckerRootSuperErrorCode::NOT_A_STATEMENT,
        .message = message,
    });
  }

  return ok(call);
}

InheritChecker::ResultSuperNotRepeated
InheritChecker::checkSuperOnce(const ClassDecl *classDecl) const {
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
InheritChecker::checkSuperArguments(const CallExpr *call, const ClassDecl *parent) const {
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
    this->walkSuperWithoutConstructor(call, classDecl->parent);
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

void InheritChecker::walkSuperWithoutConstructor(CallExpr *call, const ClassDecl *parent) {
  auto none = this->checkSuperArguments(call, parent);
  if (none.isErr()) {
    this->state.getDiagnostics().error(call->loc(), none.error().message);
  }
  this->argumentChecker->walkArgsLoosely(call);
}

ClassDecl *InheritChecker::superCallClass(const CallExpr *call) {
  auto *currentDecl = this->state.getFunctionStack().getCurrentDecl();
  auto scope = this->checkSuperAllowed(currentDecl);
  if (scope.isErr()) {
    this->state.getDiagnostics().error(call->loc(), scope.error().message);
    return nullptr;
  }
  ClassDecl *classDecl = scope.value()->cls();
  auto parent = this->checkParentClass(classDecl);
  if (parent.isErr() && parent.error().code != InheritCheckerParentClassErrorCode::BASE_ONLY) {
    this->state.getDiagnostics().error(call->loc(), parent.error().message);
  }
  if (parent.isErr()) {
    return nullptr;
  }
  auto statement = this->checkRootSuper(call);
  if (statement.isErr()) {
    this->state.getDiagnostics().error(call->loc(), statement.error().message);
    return nullptr;
  }
  auto once = this->checkSuperOnce(classDecl);
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
  auto constructorName = ObjectMemberTable::getName(ObjectMember::CONSTRUCTOR);
  auto accessible =
      this->classMemberChecker->checkAccess(chosen->cls(), chosen->access(), constructorName);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(call->loc(), accessible.error().message);
  }
  call->target = SuperCall{.method = function};
  std::vector<bool> given(isChecked ? call->args.size() : 0, true);
  this->argumentChecker->checkArguments(call, function->params, function->minArgs,
                                        function->hasRest, call->loc(), &given);
}

InheritChecker::ResultSuperMethodScope
InheritChecker::checkSuperContext(FunctionDecl *method) const {
  if (method == nullptr || (method->cls()->parent == nullptr && !method->cls()->base.has_value())) {
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
InheritChecker::checkObjectComplete(const MemberExpr *member) const {
  const Flow &flow = this->state.getFlowState().getFlow();
  bool isEarly = this->classChecker->isInitThis(member->object).isOk() &&
                 !this->state.getFlowState().isBeforeSuper() &&
                 this->classChecker->checkThisReady(flow).isErr();
  if (isEarly) {
    auto unset = this->classChecker->firstUnset(flow);
    auto message = ErrorTextSuperMethod::early(member->name, unset.value());

    return err(InheritCheckerObjectCompleteError{
        .code = InheritCheckerObjectCompleteErrorCode::CALLED_BEFORE_SET,
        .message = message,
    });
  }

  return ok(member);
}

InheritChecker::ResultParentHasMethods
InheritChecker::checkParentMethods(const ClassDecl *parent, llvm::StringRef name) const {
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

InheritChecker::ResultMethodHasItsBody InheritChecker::checkMethodBody(FuncSymbol *function,
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
  auto scope = this->checkSuperContext(method);
  if (scope.isErr() && scope.error().code != InheritCheckerSuperContextErrorCode::BASE_ONLY) {
    this->state.getDiagnostics().error(member->object->loc(), scope.error().message);
  }
  if (scope.isErr()) {
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  ClassDecl *parent = method->cls()->parent;
  if (!this->prepareSuperObject(member)) {
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  auto complete = this->checkObjectComplete(member);
  if (complete.isErr()) {
    this->state.getDiagnostics().error(member->loc(), complete.error().message);
  }
  auto found = this->checkParentMethods(parent, member->name);
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
  Expr *emitter =
      this->eventMaps->isEmitterClass(parent)
          ? this->emitterChecker->checkEmitterCall(call, member, parent, CallReceiver::SUPER)
          : nullptr;
  if (emitter != nullptr) {
    return emitter;
  }

  return this->callSuperMethod(call, member, methods);
}

FunctionDecl *InheritChecker::enclosingMethod() {
  auto functions = this->state.getFunctionStack().getFunctions();
  for (FunctionDecl *functionDecl : llvm::reverse(functions)) {
    if (!functionDecl->isArrow()) {
      return this->methodWithThis(functionDecl);
    }
  }

  return nullptr;
}

FunctionDecl *InheritChecker::methodWithThis(FunctionDecl *functionDecl) const {
  bool isMethod = functionDecl->takesThis() && functionDecl->cls()->literal() == nullptr;

  return isMethod ? functionDecl : nullptr;
}

bool InheritChecker::prepareSuperObject(MemberExpr *member) {
  auto *self = this->state.getAst().make<NameExpr>(member->object->loc(),
                                                   KeywordTable::getName(Keyword::THIS));
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
  auto body = this->checkMethodBody(picked.value().function, member->name);
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
