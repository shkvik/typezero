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
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

const char *accessWord(Access access) {
  return access == Access::Private ? "private" : "protected";
}

}

ClassMemberChecker::ClassMemberChecker(Checker &checker) : checker_(checker) {}

bool ClassMemberChecker::checkAccess(const ClassDecl *cls, Access access, llvm::StringRef name,
                                     llvm::SMLoc loc, const Expr *receiver) {
  if (access == Access::Public || Checker::sameClass(checker_.currentClass_, cls)) {
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
  if (access == Access::Protected && derives(checker_.currentClass_, cls)) {
    while (auto *cast = dyn_cast_or_null<CastExpr>(receiver)) {
      receiver = cast->operand;
    }
    auto *self = dyn_cast_or_null<NameExpr>(receiver);
    const ClassDecl *other =
        receiver && receiver->type ? ClassDeclarer::classOf(receiver->type) : nullptr;
    if (!receiver || (self && (self->name == "this" || self->name == "super")) || !other ||
        derives(other, checker_.currentClass_)) {
      return true;
    }
    checker_.diags_.error(loc,
                          "'" + name.str() + "' is protected in class '" + cls->name.str() +
                              "'; '" + checker_.currentClass_->name.str() +
                              "' may use it only on its own objects, and this one is of class '" +
                              other->name.str() + "'");
    return false;
  }
  std::string message =
      "'" + name.str() + "' is " + accessWord(access) + " in class '" + cls->name.str() + "'";
  if (access == Access::Protected) {
    message += "; only '" + cls->name.str() + "' and the classes that extend it can use it";
  }
  checker_.diags_.error(loc, message);

  return false;
}

Expr *ClassMemberChecker::checkClassMember(MemberExpr *member, const Type *expected) {
  ClassDecl *cls = ClassDeclarer::classOf(member->object->type);
  checker_.classDeclarer().completeClass(cls);
  member->type = Type::error();
  llvm::StringRef name = member->name;
  if (Expr *date = checker_.fsDeclarer().checkStatsDate(member, cls)) {
    return date;
  }
  bool viaThis = checker_.classChecker().isInitThis(member->object);
  const ClassDecl *owner = nullptr;
  const ClassField *field = ClassBases::findField(cls, name, &owner);
  if (field && !field->method) {
    checkAccess(owner, field->access, name, member->loc(), member->object);
    if (viaThis && owner == checker_.initFn_->cls &&
        !llvm::is_contained(checker_.flow_.setFields, name)) {
      checker_.diags_.error(member->loc(), "field '" + name + "' is read before it is set");
      return member;
    }
    if (!field->resolved) {
      checker_.diags_.error(member->loc(),
                            "the type of field '" + name +
                                "' comes from its initializer, which runs later; declare "
                                "it: '" +
                                name + ": T = ...'");
      return member;
    }
    member->type = field->resolved;
    if (Expr *narrowed = checker_.fieldFacts().narrowedField(member); narrowed != member) {
      return narrowed;
    }
    if (field->resolved->isOptional()) {
      if (std::optional<Checker::FieldPath> path = checker_.fieldFacts().fieldPath(member);
          path && checker_.fieldFacts().isFieldDefined(*path)) {
        return checker_.typeRelations().convert(member, field->resolved->inner());
      }
    }
    return member;
  }
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, false);
  if (methods.empty()) {
    if (!ClassDeclarer::methodsNamed(cls, name, true).empty() ||
        llvm::any_of(cls->fields,
                     [&](const ClassField &f) { return f.isStatic && f.name == name; })) {
      checker_.diags_.error(member->loc(), "'" + name + "' is a static member of '" + cls->name +
                                               "': use it through the class, '" + cls->name + "." +
                                               name + "'");
    } else {
      checker_.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                               cls->type->name() + "'");
    }
    return member;
  }
  checkAccess(methods[0]->cls, methods[0]->access, name, member->loc(), member->object);
  if (methods[0]->cls->library && checker_.eventMaps().isEmitterClass(cls) &&
      !checker_.libraryCalls().inLibrary(member->loc()) && name != "getMaxListeners" &&
      name != "eventNames") {
    checker_.emitterChecker().reportEmitterMethodValue(member);

    return member;
  }
  if (viaThis && !checker_.flow_.beforeSuper &&
      !checker_.classChecker().thisReady(checker_.flow_)) {
    checker_.diags_.error(
        member->loc(), "'this." + name + "' is used before every field is set: field '" +
                           checker_.classChecker().firstUnset(checker_.flow_) + "' is not set yet");
  }
  FuncSymbol *sym = checker_.overloadResolver().variantForType(methods, expected, member);
  if (!sym) {
    return member;
  }
  if (sym->isGeneric()) {
    checker_.diags_.error(
        member->loc(), "'" + name +
                           "' is a generic method, so it can only be called: each "
                           "call runs a copy of it made for its types; wrap it in an arrow, e.g. "
                           "'(x) => o." +
                           name + "(x)'");
    return member;
  }
  if (FunctionDecl *impl = ClassLinks::soleImplementation(cls, sym->decl)) {
    member->method = impl->sym;
  } else {
    member->method = sym, member->virtualCall = true;
  }
  member->type = checker_.returnInferrer().functionType(sym);

  return member;
}

const Type *ClassMemberChecker::checkFieldTarget(MemberExpr *member, bool compound) {
  const Type *type = member->object->type;
  ClassDecl *cls = ClassDeclarer::classOf(type);
  if (!cls) {
    return checker_.objectChecker().checkObjectFieldTarget(member);
  }
  checker_.classDeclarer().completeClass(cls);
  llvm::StringRef name = member->name;
  const ClassDecl *owner = nullptr;
  const ClassField *field = ClassBases::findField(cls, name, &owner);
  if (cls == checker_.statsClass_ && checker_.fsDeclarer().checkStatsDate(member, cls)) {
    checker_.diags_.error(member->loc(),
                          "'" + name + "' cannot be assigned: it is made from '" + name +
                              "Ms' when it is read; keep the date in a variable of its own");
    return Type::error();
  }
  if (!field || field->method) {
    std::string setter = "set" + std::string(1, llvm::toUpper(name[0])) + name.drop_front().str();
    bool method = field || !ClassDeclarer::methodsNamed(cls, name, false).empty();
    if (method && cls->library && !ClassDeclarer::methodsNamed(cls, setter, false).empty()) {
      checker_.diags_.error(member->loc(), "'" + name + "' is a method of '" + type->name() +
                                               "'; change it with '" + setter + "(...)'");
    } else if (method && cls->library) {
      checker_.diags_.error(member->loc(), "'" + name + "' is a method of '" + type->name() +
                                               "' and cannot be assigned: the object changes "
                                               "what it gives itself");
    } else if (method) {
      checker_.diags_.error(member->loc(),
                            "'" + name + "' is a method of '" + type->name() +
                                "' and cannot be replaced: a method belongs to the class, "
                                "not to the object; to change what it does, declare a "
                                "field that holds a function: '" +
                                name + ": () => void = () => { ... }'");
    } else {
      checker_.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                               type->name() + "'");
    }
    return Type::error();
  }
  checkAccess(owner, field->access, name, member->loc(), member->object);
  bool viaThis = checker_.classChecker().isInitThis(member->object);
  if (compound && viaThis && owner == checker_.initFn_->cls &&
      !llvm::is_contained(checker_.flow_.setFields, name)) {
    checker_.diags_.error(member->loc(), "field '" + name + "' is read before it is set");
    return Type::error();
  }
  if (field->readonly && !(viaThis && owner == checker_.initFn_->cls)) {
    checker_.diags_.error(member->loc(), "cannot assign to '" + name +
                                             "' because it is a read-only field of '" +
                                             type->name() + "'; only its constructor can set it");
  }
  if (!field->resolved) {
    if (viaThis && checker_.initFn_ == cls->init && owner == cls && !compound) {
      member->type = nullptr;
      return nullptr;
    }
    checker_.diags_.error(member->loc(),
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
