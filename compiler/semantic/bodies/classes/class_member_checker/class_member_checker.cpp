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
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

using llvm::cast;
using llvm::dyn_cast_or_null;

namespace tz {

const char *ClassMemberChecker::accessWord(Access access) {
  return access == Access::Private ? "private" : "protected";
}

ClassMemberChecker::ClassMemberChecker(SemanticState &owner)
    : state(owner) {}

bool ClassMemberChecker::sameClass(const ClassDecl *a, const ClassDecl *b) {
  return a && b && a->staticOwner() == b->staticOwner();
}

ClassMemberChecker::ResultMemberAccessible ClassMemberChecker::checkAccess(const ClassDecl *cls,
                                                                           Access access,
                                                                           llvm::StringRef name,
                                                                           const Expr *receiver) {
  if (access == Access::Public ||
      ClassMemberChecker::sameClass(this->state.getClassContext().getCurrentClass(), cls)) {
    return ok(cls);
  }
  auto derives = [](const ClassDecl *from, const ClassDecl *base) {
    for (const ClassDecl *c = from; c; c = c->parent) {
      if (ClassMemberChecker::sameClass(c, base)) {
        return true;
      }
    }

    return false;
  };
  if (access == Access::Protected &&
      derives(this->state.getClassContext().getCurrentClass(), cls)) {
    while (auto *cast = dyn_cast_or_null<CastExpr>(receiver)) {
      receiver = cast->operand;
    }
    auto *self = dyn_cast_or_null<NameExpr>(receiver);
    const ClassDecl *other =
        receiver && receiver->type ? ClassDeclarer::classOf(receiver->type) : nullptr;
    if (!receiver || (self && (self->name == "this" || self->name == "super")) || !other ||
        derives(other, this->state.getClassContext().getCurrentClass())) {
      return ok(cls);
    }
    std::string message = ErrorTextAccess::elsewhere(
        name, cls->name, this->state.getClassContext().getCurrentClass()->name, other->name);

    return err(ClassMemberCheckerCheckAccessError{
        .code = ClassMemberCheckerCheckAccessErrorCode::PROTECTED_ELSEWHERE,
        .message = message,
    });
  }
  const char *word = ClassMemberChecker::accessWord(access);
  if (access == Access::Protected) {
    std::string message = ErrorTextAccess::hidden(name, word, cls->name, true);

    return err(ClassMemberCheckerCheckAccessError{
        .code = ClassMemberCheckerCheckAccessErrorCode::PROTECTED_MEMBER,
        .message = message,
    });
  }
  std::string message = ErrorTextAccess::hidden(name, word, cls->name, false);

  return err(ClassMemberCheckerCheckAccessError{
      .code = ClassMemberCheckerCheckAccessErrorCode::PRIVATE_MEMBER,
      .message = message,
  });
}

ClassMemberChecker::ResultFieldIsSetBefore
ClassMemberChecker::fieldSet(const ClassDecl *owner, llvm::StringRef name, bool viaThis) const {
  bool unset = viaThis && owner == this->state.getClassContext().getInitializer()->cls() &&
               !this->state.getFlowState().isFieldSet(name);
  if (unset) {
    std::string message = ErrorTextClassField::readEarly(name);

    return err(ClassMemberCheckerFieldSetError{
        .code = ClassMemberCheckerFieldSetErrorCode::READ_BEFORE_SET,
        .message = message,
    });
  }

  return ok(name);
}

ClassMemberChecker::ResultFieldTypeIsKnown ClassMemberChecker::fieldTyped(const ClassField *field,
                                                                          llvm::StringRef name) {
  if (!field->resolved) {
    std::string message = ErrorTextClassField::laterType(name);

    return err(ClassMemberCheckerFieldTypedError{
        .code = ClassMemberCheckerFieldTypedErrorCode::TYPE_FROM_INITIALIZER,
        .message = message,
    });
  }

  return ok(field->resolved);
}

ClassMemberChecker::ResultMethodsOfObjects
ClassMemberChecker::instanceMethods(const ClassDecl *cls, llvm::StringRef name) {
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, false);
  bool staticMember = !ClassDeclarer::methodsNamed(cls, name, true).empty() ||
                      llvm::any_of(cls->fields, [&](const ClassField &f) {
                        return f.isStatic() && f.name == name;
                      });
  if (methods.empty() && staticMember) {
    std::string message = ErrorTextClassField::isStatic(name, cls->name);

    return err(ClassMemberCheckerInstanceMethodsError{
        .code = ClassMemberCheckerInstanceMethodsErrorCode::STATIC_MEMBER,
        .message = message,
    });
  }
  if (methods.empty()) {
    std::string message = ErrorTextClassField::noProperty(name, cls->type->name());

    return err(ClassMemberCheckerInstanceMethodsError{
        .code = ClassMemberCheckerInstanceMethodsErrorCode::NO_SUCH_PROPERTY,
        .message = message,
    });
  }

  return ok(methods);
}

ClassMemberChecker::ResultObjectReadyToUse
ClassMemberChecker::objectReady(bool viaThis, llvm::StringRef name) const {
  bool early = viaThis && !this->state.getFlowState().isBeforeSuper() &&
               ClassChecker(this->state).thisReady(this->state.getFlowState().getFlow()).isErr();
  if (early) {
    std::string field =
        ClassChecker(this->state).firstUnset(this->state.getFlowState().getFlow()).value();
    std::string message = ErrorTextClassField::earlyThis(name, field);

    return err(ClassMemberCheckerObjectReadyError{
        .code = ClassMemberCheckerObjectReadyErrorCode::USED_BEFORE_SET,
        .message = message,
    });
  }

  return ok(name);
}

ClassMemberChecker::ResultPlainMethodValue ClassMemberChecker::methodValue(const FuncSymbol *sym,
                                                                           llvm::StringRef name) {
  if (sym->isGeneric()) {
    std::string message = ErrorTextClassField::genericVal(name);

    return err(ClassMemberCheckerMethodValueError{
        .code = ClassMemberCheckerMethodValueErrorCode::GENERIC_METHOD,
        .message = message,
    });
  }

  return ok(sym);
}

Expr *ClassMemberChecker::checkClassMember(MemberExpr *member, const Type *expected) {
  ClassDecl *cls = ClassDeclarer::classOf(member->object->type);
  ClassDeclarer(this->state).completeClass(cls);
  TzModulesChecker(this->state).noteTlsMethod(member, cls);
  member->type = Type::error();
  llvm::StringRef name = member->name;
  if (Expr *date = FsDeclarer(this->state).checkStatsDate(member, cls)) {
    return date;
  }
  bool viaThis = ClassChecker(this->state).isInitThis(member->object).isOk();
  const ClassDecl *owner = nullptr;
  const ClassField *field = ClassBases::findField(cls, name, &owner);
  if (field && !field->method()) {
    ResultMemberAccessible accessible =
        this->checkAccess(owner, field->access, name, member->object);
    if (accessible.isErr()) {
      this->state.getDiagnostics().error(member->loc(), accessible.error().message);
    }
    ResultFieldIsSetBefore set = this->fieldSet(owner, name, viaThis);
    if (set.isErr()) {
      this->state.getDiagnostics().error(member->loc(), set.error().message);
      return member;
    }
    ResultFieldTypeIsKnown typed = ClassMemberChecker::fieldTyped(field, name);
    if (typed.isErr()) {
      this->state.getDiagnostics().error(member->loc(), typed.error().message);
      return member;
    }
    member->type = typed.value();
    if (Expr *narrowed = FieldFacts(this->state).narrowedField(member); narrowed != member) {
      return narrowed;
    }
    if (field->resolved->isOptional()) {
      if (std::optional<FieldPath> path = FieldFacts(this->state).fieldPath(member);
          path && FieldFacts(this->state).isFieldDefined(*path)) {
        return TypeRelations(this->state).convert(member, field->resolved->inner());
      }
    }
    return member;
  }
  ResultMethodsOfObjects found = ClassMemberChecker::instanceMethods(cls, name);
  if (found.isErr()) {
    this->state.getDiagnostics().error(member->loc(), found.error().message);
    return member;
  }
  std::vector<FunctionDecl *> methods = found.value();
  ResultMemberAccessible accessible =
      this->checkAccess(methods[0]->cls(), methods[0]->access(), name, member->object);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
  }
  if (methods[0]->cls()->library() && EventMaps(this->state).isEmitterClass(cls) &&
      !LibraryCalls(this->state).inLibrary(member->loc()) && name != "eventNames") {
    EmitterChecker(this->state).reportEmitterMethodValue(member);

    return member;
  }
  ResultObjectReadyToUse ready = this->objectReady(viaThis, name);
  if (ready.isErr()) {
    this->state.getDiagnostics().error(member->loc(), ready.error().message);
  }
  OverloadResolver::ResultVariantOfType variant =
      OverloadResolver(this->state).variantForType(methods, expected, member);
  if (variant.isErr()) {
    this->state.getDiagnostics().error(member->loc(), variant.error().message);
    return member;
  }
  FuncSymbol *sym = variant.value();
  ResultPlainMethodValue plain = ClassMemberChecker::methodValue(sym, name);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(member->loc(), plain.error().message);
    return member;
  }
  if (FunctionDecl *impl = ClassLinks::soleImplementation(cls, sym->decl)) {
    member->form = MethodMember{.method = impl->sym};
  } else {
    member->form = MethodMember{.method = sym, .virtualCall = true};
  }
  member->type = ReturnInferrer(this->state).functionType(sym);

  return member;
}

ClassMemberChecker::ResultStoredFieldValue ClassMemberChecker::statsDate(ClassDecl *cls,
                                                                         MemberExpr *member) {
  bool derived = this->state.getFileSystemStore().isStatsClass(cls) &&
                 FsDeclarer(this->state).checkStatsDate(member, cls);
  if (derived) {
    std::string message = ErrorTextFieldTarget::statsDate(member->name);

    return err(ClassMemberCheckerStatsDateError{
        .code = ClassMemberCheckerStatsDateErrorCode::DERIVED_DATE,
        .message = message,
    });
  }

  return ok(cls);
}

ClassMemberChecker::ResultAssignableMember
ClassMemberChecker::assignableField(const ClassField *field, const ClassDecl *cls,
                                    llvm::StringRef name, const Type *type) {
  bool assignable = field && !field->method();
  if (!assignable) {
    std::string setter = "set" + std::string(1, llvm::toUpper(name[0])) + name.drop_front().str();
    bool method = field || !ClassDeclarer::methodsNamed(cls, name, false).empty();
    bool hasSetter = !ClassDeclarer::methodsNamed(cls, setter, false).empty();
    if (method && cls->library() && hasSetter) {
      std::string message = ErrorTextFieldTarget::setter(name, type->name(), setter);

      return err(ClassMemberCheckerAssignableFieldError{
          .code = ClassMemberCheckerAssignableFieldErrorCode::SETTER_METHOD,
          .message = message,
      });
    }
    if (method && cls->library()) {
      std::string message = ErrorTextFieldTarget::library(name, type->name());

      return err(ClassMemberCheckerAssignableFieldError{
          .code = ClassMemberCheckerAssignableFieldErrorCode::LIBRARY_METHOD,
          .message = message,
      });
    }
    if (method) {
      std::string message = ErrorTextFieldTarget::method(name, type->name());

      return err(ClassMemberCheckerAssignableFieldError{
          .code = ClassMemberCheckerAssignableFieldErrorCode::CLASS_METHOD,
          .message = message,
      });
    }
    std::string message = ErrorTextClassField::noProperty(name, type->name());

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
  bool initializing = viaThis && owner == this->state.getClassContext().getInitializer()->cls();
  if (field->readonly && !initializing) {
    std::string message = ErrorTextFieldTarget::readOnly(field->name, type->name());

    return err(ClassMemberCheckerFieldWritableError{
        .code = ClassMemberCheckerFieldWritableErrorCode::READ_ONLY,
        .message = message,
    });
  }

  return ok(field);
}

const Type *ClassMemberChecker::checkFieldTarget(MemberExpr *member, bool compound) {
  const Type *type = member->object->type;
  ClassDecl *cls = ClassDeclarer::classOf(type);
  if (!cls) {
    return ObjectChecker(this->state).checkObjectFieldTarget(member);
  }
  ClassDeclarer(this->state).completeClass(cls);
  llvm::StringRef name = member->name;
  const ClassDecl *owner = nullptr;
  const ClassField *field = ClassBases::findField(cls, name, &owner);
  ResultStoredFieldValue stored = this->statsDate(cls, member);
  if (stored.isErr()) {
    this->state.getDiagnostics().error(member->loc(), stored.error().message);
    return Type::error();
  }
  ResultAssignableMember assignable = ClassMemberChecker::assignableField(field, cls, name, type);
  if (assignable.isErr()) {
    this->state.getDiagnostics().error(member->loc(), assignable.error().message);
    return Type::error();
  }
  ResultMemberAccessible accessible = this->checkAccess(owner, field->access, name, member->object);
  if (accessible.isErr()) {
    this->state.getDiagnostics().error(member->loc(), accessible.error().message);
  }
  bool viaThis = ClassChecker(this->state).isInitThis(member->object).isOk();
  if (compound) {
    ResultFieldIsSetBefore set = this->fieldSet(owner, name, viaThis);
    if (set.isErr()) {
      this->state.getDiagnostics().error(member->loc(), set.error().message);
      return Type::error();
    }
  }
  ResultFieldNotReadOnly writable = this->fieldWritable(field, owner, viaThis, type);
  if (writable.isErr()) {
    this->state.getDiagnostics().error(member->loc(), writable.error().message);
  }
  if (!field->resolved && viaThis && this->state.getClassContext().getInitializer() == cls->init &&
      owner == cls && !compound) {
    member->type = nullptr;
    return nullptr;
  }
  ResultFieldTypeIsKnown typed = ClassMemberChecker::fieldTyped(field, name);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(member->loc(), typed.error().message);
    return Type::error();
  }
  member->type = typed.value();

  return typed.value();
}

}
