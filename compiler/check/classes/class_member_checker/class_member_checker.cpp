#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/overload_resolver/overload_resolver.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

using llvm::cast;
using llvm::dyn_cast_or_null;

namespace tz {

const char *ClassMemberChecker::accessWord(Access access) {
  return access == Access::Private ? "private" : "protected";
}

ClassMemberChecker::ClassMemberChecker(Checker &owner)
    : checker(owner) {}

bool ClassMemberChecker::checkAccess(const ClassDecl *cls, Access access, llvm::StringRef name,
                                     llvm::SMLoc loc, const Expr *receiver) {
  if (access == Access::Public || Checker::sameClass(this->checker.currentClass_, cls)) {
    return true;
  }
  auto derives = [](const ClassDecl *from, const ClassDecl *base) {
    for (const ClassDecl *c = from; c; c = c->parent) {
      if (Checker::sameClass(c, base)) {
        return true;
      }
    }

    return false;
  };
  if (access == Access::Protected && derives(this->checker.currentClass_, cls)) {
    while (auto *cast = dyn_cast_or_null<CastExpr>(receiver)) {
      receiver = cast->operand;
    }
    auto *self = dyn_cast_or_null<NameExpr>(receiver);
    const ClassDecl *other =
        receiver && receiver->type ? ClassDeclarer::classOf(receiver->type) : nullptr;
    if (!receiver || (self && (self->name == "this" || self->name == "super")) || !other ||
        derives(other, this->checker.currentClass_)) {
      return true;
    }
    this->checker.diags_.error(
        loc, "'" + name.str() + "' is protected in class '" + cls->name.str() + "'; '" +
                 this->checker.currentClass_->name.str() +
                 "' may use it only on its own objects, and this one is of class '" +
                 other->name.str() + "'");
    return false;
  }
  std::string message = "'" + name.str() + "' is " + ClassMemberChecker::accessWord(access) +
                        " in class '" + cls->name.str() + "'";
  if (access == Access::Protected) {
    message += "; only '" + cls->name.str() + "' and the classes that extend it can use it";
  }
  this->checker.diags_.error(loc, message);

  return false;
}

Expr *ClassMemberChecker::checkClassMember(MemberExpr *member, const Type *expected) {
  ClassDecl *cls = ClassDeclarer::classOf(member->object->type);
  this->checker.classDeclarer().completeClass(cls);
  this->checker.tzModulesChecker().noteTlsMethod(member, cls);
  member->type = Type::error();
  llvm::StringRef name = member->name;
  if (Expr *date = this->checker.fsDeclarer().checkStatsDate(member, cls)) {
    return date;
  }
  bool viaThis = this->checker.classChecker().isInitThis(member->object).isOk();
  const ClassDecl *owner = nullptr;
  const ClassField *field = ClassBases::findField(cls, name, &owner);
  if (field && !field->method()) {
    this->checkAccess(owner, field->access, name, member->loc(), member->object);
    if (viaThis && owner == this->checker.initFn_->cls() &&
        !llvm::is_contained(this->checker.flow_.setFields, name)) {
      this->checker.diags_.error(member->loc(), "field '" + name + "' is read before it is set");
      return member;
    }
    if (!field->resolved) {
      this->checker.diags_.error(member->loc(),
                                 "the type of field '" + name +
                                     "' comes from its initializer, which runs later; declare "
                                     "it: '" +
                                     name + ": T = ...'");
      return member;
    }
    member->type = field->resolved;
    if (Expr *narrowed = this->checker.fieldFacts().narrowedField(member); narrowed != member) {
      return narrowed;
    }
    if (field->resolved->isOptional()) {
      if (std::optional<Checker::FieldPath> path = this->checker.fieldFacts().fieldPath(member);
          path && this->checker.fieldFacts().isFieldDefined(*path)) {
        return this->checker.typeRelations().convert(member, field->resolved->inner());
      }
    }
    return member;
  }
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, false);
  if (methods.empty()) {
    if (!ClassDeclarer::methodsNamed(cls, name, true).empty() ||
        llvm::any_of(cls->fields,
                     [&](const ClassField &f) { return f.isStatic() && f.name == name; })) {
      this->checker.diags_.error(member->loc(), "'" + name + "' is a static member of '" +
                                                    cls->name + "': use it through the class, '" +
                                                    cls->name + "." + name + "'");
    } else {
      this->checker.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                                    cls->type->name() + "'");
    }
    return member;
  }
  this->checkAccess(methods[0]->cls(), methods[0]->access(), name, member->loc(), member->object);
  if (methods[0]->cls()->library() && this->checker.eventMaps().isEmitterClass(cls) &&
      !this->checker.libraryCalls().inLibrary(member->loc()) && name != "eventNames") {
    this->checker.emitterChecker().reportEmitterMethodValue(member);

    return member;
  }
  if (viaThis && !this->checker.flow_.beforeSuper &&
      this->checker.classChecker().thisReady(this->checker.flow_).isErr()) {
    this->checker.diags_.error(
        member->loc(), "'this." + name + "' is used before every field is set: field '" +
                           this->checker.classChecker().firstUnset(this->checker.flow_).value() +
                           "' is not set yet");
  }
  OverloadResolver::ResultVariantOfType variant =
      this->checker.overloadResolver().variantForType(methods, expected, member);
  if (variant.isErr()) {
    this->checker.diags_.error(member->loc(), variant.error().message);
    return member;
  }
  FuncSymbol *sym = variant.value();
  if (sym->isGeneric()) {
    this->checker.diags_.error(
        member->loc(), "'" + name +
                           "' is a generic method, so it can only be called: each "
                           "call runs a copy of it made for its types; wrap it in an arrow, e.g. "
                           "'(x) => o." +
                           name + "(x)'");
    return member;
  }
  if (FunctionDecl *impl = ClassLinks::soleImplementation(cls, sym->decl)) {
    member->form = MethodMember{.method = impl->sym};
  } else {
    member->form = MethodMember{.method = sym, .virtualCall = true};
  }
  member->type = this->checker.returnInferrer().functionType(sym);

  return member;
}

const Type *ClassMemberChecker::checkFieldTarget(MemberExpr *member, bool compound) {
  const Type *type = member->object->type;
  ClassDecl *cls = ClassDeclarer::classOf(type);
  if (!cls) {
    return this->checker.objectChecker().checkObjectFieldTarget(member);
  }
  this->checker.classDeclarer().completeClass(cls);
  llvm::StringRef name = member->name;
  const ClassDecl *owner = nullptr;
  const ClassField *field = ClassBases::findField(cls, name, &owner);
  if (cls == this->checker.statsClass_ && this->checker.fsDeclarer().checkStatsDate(member, cls)) {
    this->checker.diags_.error(member->loc(),
                               "'" + name + "' cannot be assigned: it is made from '" + name +
                                   "Ms' when it is read; keep the date in a variable of its own");
    return Type::error();
  }
  if (!field || field->method()) {
    std::string setter = "set" + std::string(1, llvm::toUpper(name[0])) + name.drop_front().str();
    bool method = field || !ClassDeclarer::methodsNamed(cls, name, false).empty();
    if (method && cls->library() && !ClassDeclarer::methodsNamed(cls, setter, false).empty()) {
      this->checker.diags_.error(member->loc(), "'" + name + "' is a method of '" + type->name() +
                                                    "'; change it with '" + setter + "(...)'");
    } else if (method && cls->library()) {
      this->checker.diags_.error(member->loc(), "'" + name + "' is a method of '" + type->name() +
                                                    "' and cannot be assigned: the object changes "
                                                    "what it gives itself");
    } else if (method) {
      this->checker.diags_.error(member->loc(),
                                 "'" + name + "' is a method of '" + type->name() +
                                     "' and cannot be replaced: a method belongs to the class, "
                                     "not to the object; to change what it does, declare a "
                                     "field that holds a function: '" +
                                     name + ": () => void = () => { ... }'");
    } else {
      this->checker.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                                    type->name() + "'");
    }
    return Type::error();
  }
  this->checkAccess(owner, field->access, name, member->loc(), member->object);
  bool viaThis = this->checker.classChecker().isInitThis(member->object).isOk();
  if (compound && viaThis && owner == this->checker.initFn_->cls() &&
      !llvm::is_contained(this->checker.flow_.setFields, name)) {
    this->checker.diags_.error(member->loc(), "field '" + name + "' is read before it is set");
    return Type::error();
  }
  if (field->readonly && !(viaThis && owner == this->checker.initFn_->cls())) {
    this->checker.diags_.error(
        member->loc(), "cannot assign to '" + name + "' because it is a read-only field of '" +
                           type->name() + "'; only its constructor can set it");
  }
  if (!field->resolved) {
    if (viaThis && this->checker.initFn_ == cls->init && owner == cls && !compound) {
      member->type = nullptr;
      return nullptr;
    }
    this->checker.diags_.error(member->loc(),
                               "the type of field '" + name +
                                   "' comes from its initializer, which runs later; declare "
                                   "it: '" +
                                   name + ": T = ...'");
    return Type::error();
  }
  member->type = field->resolved;

  return field->resolved;
}

}
