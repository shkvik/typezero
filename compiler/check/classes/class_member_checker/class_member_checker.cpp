#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/overload_resolver/overload_resolver.h"
#include "compiler/check/errors/throw_checker/throw_checker.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

const char *accessWord(Access access) {
  return access == Access::Private ? "private" : "protected";
}

} // namespace

ClassMemberChecker::ClassMemberChecker(Sema &sema) : sema_(sema) {}

bool ClassMemberChecker::checkAccess(const ClassDecl *cls, Access access, llvm::StringRef name, llvm::SMLoc loc) {
  // The instances of a generic class are that class.
  if (access == Access::Public || Sema::sameClass(sema_.currentClass_, cls)) {
    return true;
  }
  // A protected member is for the class and those down its chain.
  if (access == Access::Protected) {
    for (const ClassDecl *c = sema_.currentClass_; c; c = c->parent) {
      if (Sema::sameClass(c, cls)) {
        return true;
      }
    }
  }
  std::string message =
      "'" + name.str() + "' is " + accessWord(access) + " in class '" + cls->name.str() + "'";
  if (access == Access::Protected) {
    message += "; only '" + cls->name.str() + "' and the classes that extend it can use it";
  }
  sema_.diags_.error(loc, message);

  return false;
}

Expr *ClassMemberChecker::checkClassMember(MemberExpr *member, const Type *expected) {
  ClassDecl *cls = ClassDeclarer::classOf(member->object->type);
  sema_.classDeclarer().completeClass(cls);
  member->type = Type::error();
  llvm::StringRef name = member->name;
  if (derivesFromError(cls) && name == "stack") {
    return sema_.throwChecker().checkErrorStack(member);
  }
  if (Expr *date = sema_.fsDeclarer().checkStatsDate(member, cls)) {
    return date;
  }
  bool viaThis = sema_.classChecker().isInitThis(member->object);
  const ClassDecl *owner = nullptr;
  const ClassField *field = ClassBases::findField(cls, name, &owner);
  if (field && !field->method) {
    checkAccess(owner, field->access, name, member->loc());
    // The fields up the chain are set by `super(...)`, before `this` exists.
    if (viaThis && owner == sema_.initFn_->cls && !llvm::is_contained(sema_.flow_.setFields, name)) {
      sema_.diags_.error(member->loc(), "field '" + name + "' is read before it is set");
      return member;
    }
    if (!field->resolved) {
      sema_.diags_.error(member->loc(), "the type of field '" + name +
                                      "' comes from its initializer, which runs later; declare "
                                      "it: '" +
                                      name + ": T = ...'");
      return member;
    }
    member->type = field->resolved;
    if (Expr *narrowed = sema_.fieldFacts().narrowedField(member); narrowed != member) {
      return narrowed;
    }
    if (field->resolved->isOptional()) {
      if (std::optional<Sema::FieldPath> path = sema_.fieldFacts().fieldPath(member); path && sema_.fieldFacts().isFieldDefined(*path)) {
        return sema_.typeRelations().convert(member, field->resolved->inner());
      }
    }
    return member;
  }
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, /*isStatic=*/false);
  // A property of a built-in class written in tz (sema_library.cpp).
  if (llvm::any_of(methods, [](const FunctionDecl *fn) {
        return fn->accessor != FunctionDecl::Accessor::None;
      })) {
    if (Expr *read = sema_.tzModulesChecker().checkAccessorRead(member, cls, expected)) {
      return read;
    }
  }
  if (methods.empty()) {
    if (!ClassDeclarer::methodsNamed(cls, name, /*isStatic=*/true).empty() ||
        llvm::any_of(cls->fields,
                     [&](const ClassField &f) { return f.isStatic && f.name == name; })) {
      sema_.diags_.error(member->loc(), "'" + name + "' is a static member of '" + cls->name +
                                      "': use it through the class, '" + cls->name + "." + name +
                                      "'");
    } else {
      sema_.diags_.error(member->loc(),
                   "property '" + name + "' does not exist on type '" + cls->type->name() + "'");
    }
    return member;
  }
  // A method as a value: bound to the object, so `const f = o.m; f()` works.
  checkAccess(methods[0]->cls, methods[0]->access, name, member->loc());
  // But not one of an emitter that the event map checks (sema_events.cpp).
  if (methods[0]->cls->library && sema_.eventMaps().isEmitterClass(cls) && !sema_.libraryCalls().inLibrary(member->loc()) &&
      name != "getMaxListeners" && name != "eventNames") {
    sema_.emitterChecker().reportEmitterMethodValue(member);

    return member;
  }
  if (viaThis && !sema_.flow_.beforeSuper && !sema_.classChecker().thisReady(sema_.flow_)) {
    sema_.diags_.error(member->loc(), "'this." + name + "' is used before every field is set: field '" +
                                    sema_.classChecker().firstUnset(sema_.flow_) + "' is not set yet");
  }
  FuncSymbol *sym = sema_.overloadResolver().variantForType(methods, expected, member);
  if (!sym) {
    return member;
  }
  if (sym->isGeneric()) {
    sema_.diags_.error(member->loc(),
                 "'" + name +
                     "' is a generic method, so it can only be called: each "
                     "call runs a copy of it made for its types; wrap it in an arrow, e.g. "
                     "'(x) => o." +
                     name + "(x)'");
    return member;
  }
  // The method of the object's class, if classes down the chain override it.
  if (FunctionDecl *impl = ClassLinks::soleImplementation(cls, sym->decl)) {
    member->method = impl->sym;
  } else {
    member->method = sym, member->virtualCall = true;
  }
  member->type = sema_.returnInferrer().functionType(sym);

  return member;
}

const Type *ClassMemberChecker::checkFieldTarget(MemberExpr *member, bool compound) {
  const Type *type = member->object->type;
  ClassDecl *cls = ClassDeclarer::classOf(type);
  if (!cls) {
    return sema_.objectChecker().checkObjectFieldTarget(member);
  }
  sema_.classDeclarer().completeClass(cls);
  llvm::StringRef name = member->name;
  const ClassDecl *owner = nullptr;
  const ClassField *field = ClassBases::findField(cls, name, &owner);
  if (derivesFromError(cls) && name == "stack") {
    sema_.diags_.error(member->loc(), "'stack' cannot be assigned: it is made from the places the error "
                                "passed on its way up");
    return Type::error();
  }
  if (cls == sema_.statsClass_ && sema_.fsDeclarer().checkStatsDate(member, cls)) {
    sema_.diags_.error(member->loc(), "'" + name + "' cannot be assigned: it is made from '" + name +
                                    "Ms' when it is read; keep the date in a variable of its own");
    return Type::error();
  }
  if (!field) {
    if (const Type *accessor = sema_.tzModulesChecker().checkAccessorTarget(member, cls, compound)) {
      return accessor;
    }
  }
  if (!field || field->method) {
    if (field || !ClassDeclarer::methodsNamed(cls, name, /*isStatic=*/false).empty()) {
      sema_.diags_.error(member->loc(), "'" + name + "' is a method of '" + type->name() +
                                      "' and cannot be replaced: a method belongs to the class, "
                                      "not to the object; to change what it does, declare a "
                                      "field that holds a function: '" +
                                      name + ": () => void = () => { ... }'");
    } else {
      sema_.diags_.error(member->loc(),
                   "property '" + name + "' does not exist on type '" + type->name() + "'");
    }
    return Type::error();
  }
  checkAccess(owner, field->access, name, member->loc());
  bool viaThis = sema_.classChecker().isInitThis(member->object);
  if (compound && viaThis && owner == sema_.initFn_->cls && !llvm::is_contained(sema_.flow_.setFields, name)) {
    sema_.diags_.error(member->loc(), "field '" + name + "' is read before it is set");
    return Type::error();
  }
  // A read-only field is set by the constructor of the class that declares
  // it (or declares it again).
  if (field->readonly && !(viaThis && owner == sema_.initFn_->cls)) {
    sema_.diags_.error(member->loc(), "cannot assign to '" + name +
                                    "' because it is a read-only field of '" + type->name() +
                                    "'; only its constructor can set it");
  }
  if (!field->resolved) {
    // Its type comes from this initializer (see checkAssign).
    if (viaThis && sema_.initFn_ == cls->init && owner == cls && !compound) {
      member->type = nullptr;
      return nullptr;
    }
    sema_.diags_.error(member->loc(), "the type of field '" + name +
                                    "' comes from its initializer, which runs later; declare "
                                    "it: '" +
                                    name + ": T = ...'");
    return Type::error();
  }
  member->type = field->resolved;

  return field->resolved;
}

} // namespace tz
