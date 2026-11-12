#include "compiler/semantic/bodies/classes/class_member_checker/class_member_checker.h"
#include "compiler/program/library/builtin_names/access_keyword_table/access_keyword_table.h"
#include "compiler/program/library/events/emitter_member_table/emitter_member_table.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/events/event_maps/event_maps.h"
#include "compiler/semantic/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

using llvm::cast;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr const char *kThisName = "this";
constexpr const char *kSuperName = "super";
constexpr const char *kSetterPrefix = "set";

}

ClassMemberChecker::ClassMemberChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->eventMaps = &all.eventMaps;
  this->fieldFacts = &all.fieldFacts;
  this->fsDeclarer = &all.fsDeclarer;
  this->classLinks = &all.classLinks;
  this->classBases = &all.classBases;
  this->classChecker = &all.classChecker;
  this->libraryCalls = &all.libraryCalls;
  this->classDeclarer = &all.classDeclarer;
  this->objectChecker = &all.objectChecker;
  this->typeRelations = &all.typeRelations;
  this->emitterChecker = &all.emitterChecker;
  this->returnInferrer = &all.returnInferrer;
  this->overloadResolver = &all.overloadResolver;
  this->tzModulesChecker = &all.tzModulesChecker;
}

bool ClassMemberChecker::isSameClass(const ClassDecl *first, const ClassDecl *second) const {
  return first != nullptr && second != nullptr && first->staticOwner() == second->staticOwner();
}

bool ClassMemberChecker::isDerivedFrom(const ClassDecl *from, const ClassDecl *base) const {
  for (const ClassDecl *current = from; current != nullptr; current = current->parent) {
    if (this->isSameClass(current, base)) {
      return true;
    }
  }

  return false;
}

ClassMemberChecker::ResultMemberAccessible
ClassMemberChecker::checkAccess(const ClassDecl *classDecl, Access access, llvm::StringRef name,
                                const Expr *receiver) {
  const ClassDecl *currentClass = this->state.getClassContext().getCurrentClass();
  if (access == Access::Public || this->isSameClass(currentClass, classDecl)) {
    return ok(classDecl);
  }
  if (access == Access::Protected && this->isDerivedFrom(currentClass, classDecl)) {
    return this->checkProtectedReceiver(classDecl, name, receiver);
  }
  auto word = AccessKeywordTable::getName(access);
  if (access == Access::Protected) {
    auto message = ErrorTextAccess::hiddenProtected(name, word, classDecl->name);

    return err(ClassMemberCheckerCheckAccessError{
        .code = ClassMemberCheckerCheckAccessErrorCode::PROTECTED_MEMBER,
        .message = message,
    });
  }
  auto message = ErrorTextAccess::hiddenPrivate(name, word, classDecl->name);

  return err(ClassMemberCheckerCheckAccessError{
      .code = ClassMemberCheckerCheckAccessErrorCode::PRIVATE_MEMBER,
      .message = message,
  });
}

ClassMemberChecker::ResultMemberAccessible
ClassMemberChecker::checkProtectedReceiver(const ClassDecl *classDecl, llvm::StringRef name,
                                           const Expr *receiver) const {
  const ClassDecl *currentClass = this->state.getClassContext().getCurrentClass();
  const Expr *uncast = receiver != nullptr ? ExprParts::withoutCasts(receiver) : nullptr;
  auto *self = dyn_cast_or_null<NameExpr>(uncast);
  const ClassDecl *other = uncast != nullptr && uncast->type != nullptr
                               ? this->classDeclarer->classOf(uncast->type)
                               : nullptr;
  bool isSelf = self != nullptr && (self->name == kThisName || self->name == kSuperName);
  bool isReachable =
      uncast == nullptr || isSelf || other == nullptr || this->isDerivedFrom(other, currentClass);
  if (isReachable) {
    return ok(classDecl);
  }
  auto message = ErrorTextAccess::elsewhere(name, classDecl->name, currentClass->name, other->name);

  return err(ClassMemberCheckerCheckAccessError{
      .code = ClassMemberCheckerCheckAccessErrorCode::PROTECTED_ELSEWHERE,
      .message = message,
  });
}

ClassMemberChecker::ResultFieldIsSetBefore ClassMemberChecker::checkFieldSet(const ClassDecl *owner,
                                                                             llvm::StringRef name,
                                                                             bool isViaThis) const {
  bool isUnset = isViaThis && owner == this->state.getClassContext().getInitializer()->cls() &&
                 !this->state.getFlowState().isFieldSet(name);
  if (isUnset) {
    auto message = ErrorTextClassField::readEarly(name);

    return err(ClassMemberCheckerFieldSetError{
        .code = ClassMemberCheckerFieldSetErrorCode::READ_BEFORE_SET,
        .message = message,
    });
  }

  return ok(name);
}

ClassMemberChecker::ResultFieldTypeIsKnown
ClassMemberChecker::checkFieldTyped(const ClassField *field, llvm::StringRef name) const {
  if (field->resolved == nullptr) {
    auto message = ErrorTextClassField::laterType(name);

    return err(ClassMemberCheckerFieldTypedError{
        .code = ClassMemberCheckerFieldTypedErrorCode::TYPE_FROM_INITIALIZER,
        .message = message,
    });
  }

  return ok(field->resolved);
}

ClassMemberChecker::ResultMethodsOfObjects
ClassMemberChecker::checkInstanceMethods(const ClassDecl *classDecl, llvm::StringRef name) const {
  auto methods = this->classDeclarer->methodsNamed(classDecl, name);
  auto staticMethods = this->classDeclarer->staticMethodsNamed(classDecl, name);
  bool isStaticMember = !staticMethods.empty() || this->hasStaticField(classDecl, name);
  if (methods.empty() && isStaticMember) {
    auto message = ErrorTextClassField::isStatic(name, classDecl->name);

    return err(ClassMemberCheckerInstanceMethodsError{
        .code = ClassMemberCheckerInstanceMethodsErrorCode::STATIC_MEMBER,
        .message = message,
    });
  }
  if (methods.empty()) {
    auto message = ErrorTextClassField::noProperty(name, classDecl->type->name());

    return err(ClassMemberCheckerInstanceMethodsError{
        .code = ClassMemberCheckerInstanceMethodsErrorCode::NO_SUCH_PROPERTY,
        .message = message,
    });
  }

  return ok(methods);
}

bool ClassMemberChecker::hasStaticField(const ClassDecl *classDecl, llvm::StringRef name) const {
  for (const ClassField &field : classDecl->fields) {
    if (field.isStatic() && field.name == name) {
      return true;
    }
  }

  return false;
}

ClassMemberChecker::ResultObjectReadyToUse
ClassMemberChecker::checkObjectReady(bool isViaThis, llvm::StringRef name) const {
  const Flow &flow = this->state.getFlowState().getFlow();
  bool isEarly = isViaThis && !this->state.getFlowState().isBeforeSuper() &&
                 this->classChecker->checkThisReady(flow).isErr();
  if (isEarly) {
    auto unset = this->classChecker->firstUnset(flow);
    auto message = ErrorTextClassField::earlyThis(name, unset.value());

    return err(ClassMemberCheckerObjectReadyError{
        .code = ClassMemberCheckerObjectReadyErrorCode::USED_BEFORE_SET,
        .message = message,
    });
  }

  return ok(name);
}

ClassMemberChecker::ResultPlainMethodValue
ClassMemberChecker::checkMethodValue(const FuncSymbol *function, llvm::StringRef name) const {
  if (function->isGeneric()) {
    auto message = ErrorTextClassField::genericVal(name);

    return err(ClassMemberCheckerMethodValueError{
        .code = ClassMemberCheckerMethodValueErrorCode::GENERIC_METHOD,
        .message = message,
    });
  }

  return ok(function);
}

Expr *ClassMemberChecker::checkClassMember(MemberExpr *member, const Type *expected) {
  auto *classDecl = this->classDeclarer->classOf(member->object->type);
  this->classDeclarer->completeClass(classDecl);
  this->tzModulesChecker->noteTlsMethod(member, classDecl);
  member->type = Type::error();
  auto *date = this->fsDeclarer->checkStatsDate(member, classDecl);
  if (date != nullptr) {
    return date;
  }
  bool isViaThis = this->classChecker->isInitThis(member->object).isOk();
  const ClassDecl *owner = nullptr;
  const auto *field = this->classBases->findField(classDecl, member->name, &owner);
  if (field != nullptr && field->method() == nullptr) {
    return this->checkFieldMember(member, field, owner, isViaThis);
  }

  return this->checkMethodMember(member, classDecl, expected, isViaThis);
}

Expr *ClassMemberChecker::checkFieldMember(MemberExpr *member, const ClassField *field,
                                           const ClassDecl *owner, bool isViaThis) {
  llvm::StringRef name = member->name;
  auto accessible = this->checkAccess(owner, field->access, name, member->object);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
  }
  auto set = this->checkFieldSet(owner, name, isViaThis);
  if (set.isErr()) {
    this->state.getDiagnostics().error(member->loc(), set.error().message);
    return member;
  }
  auto typed = this->checkFieldTyped(field, name);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(member->loc(), typed.error().message);
    return member;
  }
  member->type = typed.value();
  auto *narrowed = this->fieldFacts->narrowedField(member);
  if (narrowed != member) {
    return narrowed;
  }
  if (!field->resolved->isOptional()) {
    return member;
  }
  auto path = this->fieldFacts->fieldPath(member);
  bool isDefined = path.has_value() && this->fieldFacts->isFieldDefined(*path);
  if (isDefined) {
    return this->typeRelations->convert(member, field->resolved->inner());
  }

  return member;
}

Expr *ClassMemberChecker::checkMethodMember(MemberExpr *member, ClassDecl *classDecl,
                                            const Type *expected, bool isViaThis) {
  llvm::StringRef name = member->name;
  auto found = this->checkInstanceMethods(classDecl, name);
  if (found.isErr()) {
    this->state.getDiagnostics().error(member->loc(), found.error().message);
    return member;
  }
  std::vector<FunctionDecl *> methods = found.value();
  auto accessible =
      this->checkAccess(methods[0]->cls(), methods[0]->access(), name, member->object);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
  }
  bool isEmitterValue = methods[0]->cls()->library() &&
                        this->eventMaps->isEmitterClass(classDecl) &&
                        !this->libraryCalls->isInLibrary(member->loc()) &&
                        !EmitterMemberTable::isName(name, EmitterMember::EVENT_NAMES);
  if (isEmitterValue) {
    this->emitterChecker->reportEmitterMethodValue(member);

    return member;
  }
  auto ready = this->checkObjectReady(isViaThis, name);
  if (ready.isErr()) {
    this->state.getDiagnostics().error(member->loc(), ready.error().message);
  }
  auto variant = this->overloadResolver->variantForType(methods, expected, member);
  if (variant.isErr()) {
    this->state.getDiagnostics().error(member->loc(), variant.error().message);
    return member;
  }
  FuncSymbol *function = variant.value();
  auto plain = this->checkMethodValue(function, name);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(member->loc(), plain.error().message);
    return member;
  }
  member->form = this->methodForm(classDecl, function);
  member->type = this->returnInferrer->functionType(function);

  return member;
}

MethodMember ClassMemberChecker::methodForm(ClassDecl *classDecl, FuncSymbol *function) const {
  auto *implementation = this->classLinks->soleImplementation(classDecl, function->decl);
  if (implementation != nullptr) {
    return MethodMember{.method = implementation->sym};
  }

  return MethodMember{.method = function, .virtualCall = true};
}

ClassMemberChecker::ResultStoredFieldValue ClassMemberChecker::checkStatsDate(ClassDecl *classDecl,
                                                                              MemberExpr *member) {
  bool isDerived = this->state.getFileSystemStore().isStatsClass(classDecl) &&
                   this->fsDeclarer->checkStatsDate(member, classDecl) != nullptr;
  if (isDerived) {
    auto message = ErrorTextFieldTarget::statsDate(member->name);

    return err(ClassMemberCheckerStatsDateError{
        .code = ClassMemberCheckerStatsDateErrorCode::DERIVED_DATE,
        .message = message,
    });
  }

  return ok(classDecl);
}

ClassMemberChecker::ResultAssignableMember
ClassMemberChecker::checkAssignableField(const ClassField *field, const ClassDecl *classDecl,
                                         llvm::StringRef name, const Type *type) const {
  bool isAssignable = field != nullptr && field->method() == nullptr;
  if (isAssignable) {
    return ok(field);
  }
  auto setter = this->setterName(name);
  auto methods = this->classDeclarer->methodsNamed(classDecl, name);
  bool isMethod = field != nullptr || !methods.empty();
  auto setters = this->classDeclarer->methodsNamed(classDecl, setter);
  bool hasSetter = !setters.empty();
  if (isMethod && classDecl->library() && hasSetter) {
    auto message = ErrorTextFieldTarget::setter(name, type->name(), setter);

    return err(ClassMemberCheckerAssignableFieldError{
        .code = ClassMemberCheckerAssignableFieldErrorCode::SETTER_METHOD,
        .message = message,
    });
  }
  if (isMethod && classDecl->library()) {
    auto message = ErrorTextFieldTarget::library(name, type->name());

    return err(ClassMemberCheckerAssignableFieldError{
        .code = ClassMemberCheckerAssignableFieldErrorCode::LIBRARY_METHOD,
        .message = message,
    });
  }
  if (isMethod) {
    auto message = ErrorTextFieldTarget::method(name, type->name());

    return err(ClassMemberCheckerAssignableFieldError{
        .code = ClassMemberCheckerAssignableFieldErrorCode::CLASS_METHOD,
        .message = message,
    });
  }
  auto message = ErrorTextClassField::noProperty(name, type->name());

  return err(ClassMemberCheckerAssignableFieldError{
      .code = ClassMemberCheckerAssignableFieldErrorCode::NO_SUCH_PROPERTY,
      .message = message,
  });
}

std::string ClassMemberChecker::setterName(llvm::StringRef name) const {
  std::string first(1, llvm::toUpper(name[0]));

  return kSetterPrefix + first + name.drop_front().str();
}

ClassMemberChecker::ResultFieldNotReadOnly
ClassMemberChecker::checkFieldWritable(const ClassField *field, const ClassDecl *owner,
                                       bool isViaThis, const Type *type) const {
  bool isInitializing = isViaThis && owner == this->state.getClassContext().getInitializer()->cls();
  if (field->readonly && !isInitializing) {
    auto message = ErrorTextFieldTarget::readOnly(field->name, type->name());

    return err(ClassMemberCheckerFieldWritableError{
        .code = ClassMemberCheckerFieldWritableErrorCode::READ_ONLY,
        .message = message,
    });
  }

  return ok(field);
}

const Type *ClassMemberChecker::checkFieldTarget(MemberExpr *member, AssignForm form) {
  const Type *type = member->object->type;
  auto *classDecl = this->classDeclarer->classOf(type);
  if (classDecl == nullptr) {
    return this->objectChecker->checkObjectFieldTarget(member);
  }
  this->classDeclarer->completeClass(classDecl);
  llvm::StringRef name = member->name;
  const ClassDecl *owner = nullptr;
  const auto *field = this->classBases->findField(classDecl, name, &owner);
  auto stored = this->checkStatsDate(classDecl, member);
  if (stored.isErr()) {
    this->state.getDiagnostics().error(member->loc(), stored.error().message);
    return Type::error();
  }
  auto assignable = this->checkAssignableField(field, classDecl, name, type);
  if (assignable.isErr()) {
    this->state.getDiagnostics().error(member->loc(), assignable.error().message);
    return Type::error();
  }
  auto accessible = this->checkAccess(owner, field->access, name, member->object);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
  }
  bool isViaThis = this->classChecker->isInitThis(member->object).isOk();
  bool isCompound = form == AssignForm::COMPOUND;
  bool isUnsetCompound = isCompound && this->reportUnsetField(member, owner, isViaThis);
  if (isUnsetCompound) {
    return Type::error();
  }
  auto writable = this->checkFieldWritable(field, owner, isViaThis, type);
  if (writable.isErr()) {
    this->state.getDiagnostics().error(member->loc(), writable.error().message);
  }
  bool isTypedLater = field->resolved == nullptr && isViaThis &&
                      this->state.getClassContext().getInitializer() == classDecl->init &&
                      owner == classDecl && !isCompound;
  if (isTypedLater) {
    member->type = nullptr;
    return nullptr;
  }
  auto typed = this->checkFieldTyped(field, name);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(member->loc(), typed.error().message);
    return Type::error();
  }
  member->type = typed.value();

  return typed.value();
}

bool ClassMemberChecker::reportUnsetField(const MemberExpr *member, const ClassDecl *owner,
                                          bool isViaThis) {
  auto set = this->checkFieldSet(owner, member->name, isViaThis);
  if (set.isOk()) {
    return false;
  }
  this->state.getDiagnostics().error(member->loc(), set.error().message);

  return true;
}

}
