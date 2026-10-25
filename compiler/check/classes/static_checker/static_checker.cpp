#include "compiler/check/classes/static_checker/static_checker.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/classes/overload_resolver/overload_resolver.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/events/event_target_checker/event_target_checker.h"
#include "compiler/library/stream/stream_sources/stream_sources.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

// A static field of a class or of one up its chain, and the class that
// declares it.
ClassField *staticField(ClassDecl *cls, llvm::StringRef name, ClassDecl **owner) {
  // Those of an instance of a generic class are the generic class's.
  for (ClassDecl *c = cls; c; c = c->parent) {
    for (ClassField &f : c->staticOwner()->fields) {
      if (f.isStatic && f.name == name && f.var) {
        *owner = c->staticOwner();
        return &f;
      }
    }
  }

  return nullptr;
}

} // namespace

StaticChecker::StaticChecker(Checker &checker) : checker_(checker) {}

Expr *StaticChecker::checkStaticMember(MemberExpr *member, ClassDecl *cls, const Type *expected) {
  llvm::StringRef name = member->name;
  member->type = Type::error();
  // Up the chain too: a class has the static members of the one it extends.
  ClassDecl *owner = nullptr;
  if (ClassField *found = staticField(cls, name, &owner)) {
    ClassField &f = *found;
    checker_.classMemberChecker().checkAccess(owner, f.access, name, member->loc());
    auto *read = checker_.ctx_.make<NameExpr>(member->loc(), f.var->name);
    read->var = f.var;
    if (!f.var->type) {
      if (!owner->declared && checker_.loading_ && checker_.functions_.empty()) {
        checker_.diags_.error(member->loc(), "'" + f.var->name + "' is used before class '" +
                                                 owner->name + "' is declared, which sets it");
      } else {
        checker_.diags_.error(member->loc(),
                              "the type of '" + f.var->name +
                                  "' comes from its value, which is set later; declare "
                                  "it: 'static " +
                                  name + ": T = ...'");
      }
      read->type = Type::error();
      return read;
    }
    if (!owner->declared && checker_.loading_ && checker_.functions_.empty() &&
        checker_.declModules_.lookup(owner) == checker_.loading_) {
      checker_.diags_.error(member->loc(), "'" + f.var->name + "' is used before class '" +
                                               owner->name + "' is declared, which sets it");
    } else if (checker_.staticInit_ && checker_.currentClass_ == owner &&
               !checker_.staticsSet_.contains(f.var)) {
      checker_.diags_.error(member->loc(), "'" + f.var->name +
                                               "' is read before it is set: the static "
                                               "fields are set in the order they are declared");
    }
    read->type = f.var->type;
    if (read->type->isOptional() && checker_.isDefined(f.var)) {
      return checker_.typeRelations().convert(read, read->type->inner());
    }
    return read;
  }
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, /*isStatic=*/true);
  if (methods.empty() && (name == "name" || name == "prototype")) {
    // As of a class as a value: `C.name` is "C"; there is no prototype.
    auto *object = cast<NameExpr>(member->object);
    member->object = checker_.classValueChecker().checkClassValue(object, cls);
    return checker_.classValueChecker().checkClassRefMember(member);
  }
  if (methods.empty()) {
    if (ClassBases::findField(cls, name) ||
        !ClassDeclarer::methodsNamed(cls, name, /*isStatic=*/false).empty()) {
      checker_.diags_.error(
          member->loc(), "'" + name + "' is a member of the instances of '" + cls->name +
                             "', not of the class: create one with 'new " + cls->name + "(...)'");
    } else {
      checker_.diags_.error(member->loc(),
                            "class '" + cls->name + "' has no static member '" + name + "'");
    }
    return member;
  }
  checker_.classMemberChecker().checkAccess(methods[0]->cls, methods[0]->access, name,
                                            member->loc());
  FuncSymbol *sym = checker_.overloadResolver().variantForType(methods, expected, member);
  if (!sym) {
    return member;
  }
  // A static method as a value, as a top-level function is one.
  auto *value = checker_.ctx_.make<NameExpr>(member->loc(), name);
  value->func = sym;
  value->type = checker_.returnInferrer().functionType(sym);

  return value;
}

const Type *StaticChecker::checkStaticTarget(Expr *&target, ClassDecl *cls) {
  auto *member = cast<MemberExpr>(target);
  // The class's name is no value: the target is a static field, or wrong.
  member->object->type = member->type = Type::error();
  ClassDecl *owner = nullptr;
  if (ClassField *found = staticField(cls, member->name, &owner)) {
    ClassField &f = *found;
    checker_.classMemberChecker().checkAccess(owner, f.access, f.name, member->loc());
    auto *name = checker_.ctx_.make<NameExpr>(member->loc(), f.var->name);
    name->var = f.var;
    name->type = f.var->type ? f.var->type : Type::error();
    target = name;
    if (f.var->isConst) {
      checker_.diags_.error(member->loc(), "cannot assign to '" + f.var->name +
                                               "' because it is a read-only static field");
    }
    f.var->reassigned = true;
    if (!f.var->type) {
      checker_.diags_.error(member->loc(),
                            "the type of '" + f.var->name +
                                "' comes from its value, which is set later; declare it: "
                                "'static " +
                                f.name + ": T = ...'");
      return Type::error();
    }
    return f.var->type;
  }
  if (!ClassDeclarer::methodsNamed(cls, member->name, /*isStatic=*/true).empty()) {
    checker_.diags_.error(member->loc(), "'" + member->name + "' is a static method of '" +
                                             cls->name + "' and cannot be replaced");
  } else {
    checker_.diags_.error(member->loc(),
                          "class '" + cls->name + "' has no static field '" + member->name + "'");
  }

  return Type::error();
}

Expr *StaticChecker::checkClassStaticCall(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                                          const Type *expected) {
  call->type = Type::error();
  // `EventEmitter.once(emitter, name)`: the functions of node:events are
  // properties of EventEmitter too (sema_events.cpp).
  static const llvm::StringLiteral eventsFunctions[] = {
      "once", "on", "listenerCount", "getEventListeners", "getMaxListeners", "setMaxListeners"};
  if (cls->library && cls->name == "EventEmitter" &&
      llvm::is_contained(eventsFunctions, member->name)) {
    if (Expr *events =
            checker_.eventTargetChecker().checkEventsFunction(call, member->name, member->loc())) {
      return events;
    }
    return checker_.callChecker().checkDirectCall(
        call, checker_.libFunctions_.lookup(member->name)->sym, member->loc(), expected);
  }
  // `Readable.from(iterable)` (sema_stream.cpp).
  if (cls->library && cls->name == "Readable" && member->name == "from") {
    return checker_.streamSources().checkStreamFrom(call, member);
  }
  std::vector<FunctionDecl *> methods =
      ClassDeclarer::methodsNamed(cls, member->name, /*isStatic=*/true);
  if (methods.empty()) {
    // A static field that holds a function, or an error.
    call->callee = checkStaticMember(member, cls, nullptr);
    const Type *type = call->callee->type;
    if (type->isFunction()) {
      return checker_.callChecker().checkValueCall(call, type);
    }
    if (!type->isError()) {
      checker_.diags_.error(member->loc(), "'" + cls->name + "." + member->name +
                                               "' is not a function; it has type '" + type->name() +
                                               "'");
    }
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  checker_.classMemberChecker().checkAccess(methods[0]->cls, methods[0]->access, member->name,
                                            member->loc());
  bool checked = false;
  std::vector<const Type *> bound;
  FuncSymbol *sym = checker_.overloadResolver().chooseVariant(methods, call->args, call->spread,
                                                              member->loc(), checked, &bound);
  if (!sym) {
    return call;
  }
  checker_.returnInferrer().resolveParams(sym);
  if (!sym->isGeneric() && !member->typeArgs.empty()) {
    checker_.diags_.error(
        member->loc(), "'" + member->name + "' is not a generic method; remove the type arguments");
  }
  // A generic static method, as a generic function.
  if (sym->isGeneric() && checked) {
    return checker_.overloadResolver().callGenericVariant(call, sym, bound, member->loc(),
                                                          /*method=*/false);
  }
  if (sym->isGeneric()) {
    return checker_.typeArgInferrer().checkGenericCall(call, sym, member->loc(), expected,
                                                       member->typeArgs);
  }
  call->func = sym;
  call->type = checker_.returnInferrer().returnType(sym);
  // The arguments chooseVariant checked are only converted.
  std::vector<bool> given(checked ? call->args.size() : 0, true);

  return checker_.argumentChecker().checkArguments(call, sym->params, sym->minArgs, sym->hasRest,
                                                   member->loc(), &given);
}

} // namespace tz
