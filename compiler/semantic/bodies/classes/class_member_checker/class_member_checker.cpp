#include "compiler/semantic/bodies/classes/class_member_checker/class_member_checker.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
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

const char *ClassMemberChecker::accessWord(Access access) const {
  return access == Access::Private ? "private" : "protected";
}

bool ClassMemberChecker::sameClass(const ClassDecl *a, const ClassDecl *b) const {
  return a != nullptr && b != nullptr && a->staticOwner() == b->staticOwner();
}

ClassMemberChecker::ResultMemberAccessible
ClassMemberChecker::checkAccess(const ClassDecl *classDecl, Access access, llvm::StringRef name,
                                const Expr *receiver) {
  if (access == Access::Public ||
      this->sameClass(this->state.getClassContext().getCurrentClass(), classDecl)) {
    return ok(classDecl);
  }
  auto derives = [this](const ClassDecl *from, const ClassDecl *base) {
    for (const ClassDecl *c = from; c != nullptr; c = c->parent) {
      if (this->sameClass(c, base)) {
        return true;
      }
    }

    return false;
  };
  if (access == Access::Protected &&
      derives(this->state.getClassContext().getCurrentClass(), classDecl)) {
    while (auto *castExpr = dyn_cast_or_null<CastExpr>(receiver)) {
      receiver = castExpr->operand;
    }
    auto *self = dyn_cast_or_null<NameExpr>(receiver);
    const ClassDecl *other = receiver != nullptr && receiver->type != nullptr
                                 ? this->classDeclarer->classOf(receiver->type)
                                 : nullptr;
    if (receiver == nullptr ||
        (self != nullptr && (self->name == "this" || self->name == "super")) || other == nullptr ||
        derives(other, this->state.getClassContext().getCurrentClass())) {
      return ok(classDecl);
    }
    auto message = ErrorTextAccess::elsewhere(
        name, classDecl->name, this->state.getClassContext().getCurrentClass()->name, other->name);

    return err(ClassMemberCheckerCheckAccessError{
        .code = ClassMemberCheckerCheckAccessErrorCode::PROTECTED_ELSEWHERE,
        .message = message,
    });
  }
  const char *word = this->accessWord(access);
  if (access == Access::Protected) {
    auto message = ErrorTextAccess::hidden(name, word, classDecl->name, true);

    return err(ClassMemberCheckerCheckAccessError{
        .code = ClassMemberCheckerCheckAccessErrorCode::PROTECTED_MEMBER,
        .message = message,
    });
  }
  auto message = ErrorTextAccess::hidden(name, word, classDecl->name, false);

  return err(ClassMemberCheckerCheckAccessError{
      .code = ClassMemberCheckerCheckAccessErrorCode::PRIVATE_MEMBER,
      .message = message,
  });
}

ClassMemberChecker::ResultFieldIsSetBefore
ClassMemberChecker::fieldSet(const ClassDecl *owner, llvm::StringRef name, bool viaThis) const {
  bool isUnset = viaThis && owner == this->state.getClassContext().getInitializer()->cls() &&
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
ClassMemberChecker::fieldTyped(const ClassField *field, llvm::StringRef name) const {
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
ClassMemberChecker::instanceMethods(const ClassDecl *classDecl, llvm::StringRef name) const {
  auto methods = this->classDeclarer->methodsNamed(classDecl, name);
  bool isStaticMember = !this->classDeclarer->staticMethodsNamed(classDecl, name).empty() ||
                        llvm::any_of(classDecl->fields, [&](const ClassField &field) {
                          return field.isStatic() && field.name == name;
                        });
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

ClassMemberChecker::ResultObjectReadyToUse
ClassMemberChecker::objectReady(bool viaThis, llvm::StringRef name) const {
  bool isEarly = viaThis && !this->state.getFlowState().isBeforeSuper() &&
                 this->classChecker->thisReady(this->state.getFlowState().getFlow()).isErr();
  if (isEarly) {
    std::string field =
        this->classChecker->firstUnset(this->state.getFlowState().getFlow()).value();
    auto message = ErrorTextClassField::earlyThis(name, field);

    return err(ClassMemberCheckerObjectReadyError{
        .code = ClassMemberCheckerObjectReadyErrorCode::USED_BEFORE_SET,
        .message = message,
    });
  }

  return ok(name);
}

ClassMemberChecker::ResultPlainMethodValue
ClassMemberChecker::methodValue(const FuncSymbol *function, llvm::StringRef name) const {
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
                                           const ClassDecl *owner, bool viaThis) {
  llvm::StringRef name = member->name;
  auto accessible = this->checkAccess(owner, field->access, name, member->object);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
  }
  auto set = this->fieldSet(owner, name, viaThis);
  if (set.isErr()) {
    this->state.getDiagnostics().error(member->loc(), set.error().message);
    return member;
  }
  auto typed = this->fieldTyped(field, name);
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
                                            const Type *expected, bool viaThis) {
  llvm::StringRef name = member->name;
  auto found = this->instanceMethods(classDecl, name);
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
                        !this->libraryCalls->inLibrary(member->loc()) && name != "eventNames";
  if (isEmitterValue) {
    this->emitterChecker->reportEmitterMethodValue(member);

    return member;
  }
  auto ready = this->objectReady(viaThis, name);
  if (ready.isErr()) {
    this->state.getDiagnostics().error(member->loc(), ready.error().message);
  }
  auto variant = this->overloadResolver->variantForType(methods, expected, member);
  if (variant.isErr()) {
    this->state.getDiagnostics().error(member->loc(), variant.error().message);
    return member;
  }
  FuncSymbol *function = variant.value();
  auto plain = this->methodValue(function, name);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(member->loc(), plain.error().message);
    return member;
  }
  auto *implementation = this->classLinks->soleImplementation(classDecl, function->decl);
  if (implementation != nullptr) {
    member->form = MethodMember{.method = implementation->sym};
  } else {
    member->form = MethodMember{.method = function, .virtualCall = true};
  }
  member->type = this->returnInferrer->functionType(function);

  return member;
}

ClassMemberChecker::ResultStoredFieldValue ClassMemberChecker::statsDate(ClassDecl *classDecl,
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
ClassMemberChecker::assignableField(const ClassField *field, const ClassDecl *classDecl,
                                    llvm::StringRef name, const Type *type) const {
  bool isAssignable = field != nullptr && field->method() == nullptr;
  if (!isAssignable) {
    std::string setter = "set" + std::string(1, llvm::toUpper(name[0])) + name.drop_front().str();
    bool isMethod = field != nullptr || !this->classDeclarer->methodsNamed(classDecl, name).empty();
    bool hasSetter = !this->classDeclarer->methodsNamed(classDecl, setter).empty();
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

  return ok(field);
}

ClassMemberChecker::ResultFieldNotReadOnly
ClassMemberChecker::fieldWritable(const ClassField *field, const ClassDecl *owner, bool viaThis,
                                  const Type *type) const {
  bool isInitializing = viaThis && owner == this->state.getClassContext().getInitializer()->cls();
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
  bool isCompound = form == AssignForm::COMPOUND;
  const Type *type = member->object->type;
  auto *classDecl = this->classDeclarer->classOf(type);
  if (classDecl == nullptr) {
    return this->objectChecker->checkObjectFieldTarget(member);
  }
  this->classDeclarer->completeClass(classDecl);
  llvm::StringRef name = member->name;
  const ClassDecl *owner = nullptr;
  const auto *field = this->classBases->findField(classDecl, name, &owner);
  auto stored = this->statsDate(classDecl, member);
  if (stored.isErr()) {
    this->state.getDiagnostics().error(member->loc(), stored.error().message);
    return Type::error();
  }
  auto assignable = this->assignableField(field, classDecl, name, type);
  if (assignable.isErr()) {
    this->state.getDiagnostics().error(member->loc(), assignable.error().message);
    return Type::error();
  }
  auto accessible = this->checkAccess(owner, field->access, name, member->object);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
  }
  bool isViaThis = this->classChecker->isInitThis(member->object).isOk();
  if (isCompound) {
    auto set = this->fieldSet(owner, name, isViaThis);
    if (set.isErr()) {
      this->state.getDiagnostics().error(member->loc(), set.error().message);
      return Type::error();
    }
  }
  auto writable = this->fieldWritable(field, owner, isViaThis, type);
  if (writable.isErr()) {
    this->state.getDiagnostics().error(member->loc(), writable.error().message);
  }
  if (field->resolved == nullptr && isViaThis &&
      this->state.getClassContext().getInitializer() == classDecl->init && owner == classDecl &&
      !isCompound) {
    member->type = nullptr;
    return nullptr;
  }
  auto typed = this->fieldTyped(field, name);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(member->loc(), typed.error().message);
    return Type::error();
  }
  member->type = typed.value();

  return typed.value();
}

}
