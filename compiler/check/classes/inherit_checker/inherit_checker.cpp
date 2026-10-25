#include "compiler/check/classes/inherit_checker/inherit_checker.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_check_helpers/class_check_helpers.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/classes/overload_resolver/overload_resolver.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/whole_program/flows/flows.h"
#include "compiler/whole_program/wide_writes/wide_writes.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

// Whether a function type takes the parameters of `sym`.
bool takesParams(const Type *fnType, const FuncSymbol *sym) {
  return fnType->isFunction() && fnType->params() == llvm::ArrayRef<const Type *>(sym->params) &&
         fnType->minArgs() == sym->minArgs && fnType->hasRest() == sym->hasRest;
}

// An arrow in a field of a class that is compiled as a method.
bool isArrowMethod(const FunctionDecl *fn) {
  if (!fn->cls) {
    return false;
  }
  for (const ClassField &f : fn->cls->fields) {
    if (f.method == fn) {
      return true;
    }
  }

  return false;
}

// The interfaces a class implements, and those they extend, nearest first.
void interfacesOf(const ClassDecl *cls, std::vector<const Type *> &out) {
  std::vector<const Type *> queue(cls->implemented.begin(), cls->implemented.end());
  for (size_t i = 0; i < queue.size(); ++i) {
    if (llvm::is_contained(out, queue[i])) {
      continue;
    }
    out.push_back(queue[i]);
    for (const Type *base : queue[i]->bases()) {
      queue.push_back(base);
    }
  }
}

} // namespace

InheritChecker::InheritChecker(Checker &checker) : checker_(checker) {}

void InheritChecker::checkAbstract(ClassDecl *cls) {
  if (cls->literal) {
    return;
  }
  for (FunctionDecl *fn : ClassLinks::ownMethods(cls)) {
    if (fn->isAbstract && !cls->isAbstract) {
      checker_.diags_.error(fn->loc(), "abstract method '" + fn->name +
                                           "' needs an abstract class: '" + cls->name +
                                           "' can be created with 'new', and the method would "
                                           "have no body; write 'abstract class " +
                                           cls->name + "'");
    }
  }
  if (cls->isAbstract) {
    return;
  }
  std::vector<FunctionDecl *> missing;
  for (size_t slot = 0; slot < cls->vtable.size(); ++slot) {
    if (cls->vtable[slot]) {
      continue;
    }
    // The abstract method of this slot, as the nearest class declares it.
    for (const ClassDecl *c = cls; c; c = c->parent) {
      FunctionDecl *found = nullptr;
      for (FunctionDecl *fn : ClassLinks::ownMethods(c)) {
        if (fn->slot == int(slot)) {
          found = fn;
        }
      }
      if (found) {
        if (found->cls != cls) {
          missing.push_back(found);
        }
        break;
      }
    }
  }
  for (FunctionDecl *fn : missing) {
    checker_.diags_.error(cls->loc(),
                          "class '" + cls->name + "' does not implement the abstract method " +
                              variantText(fn) + " of '" + fn->cls->name +
                              "'; implement it, or make '" + cls->name + "' abstract too");
  }
}

void InheritChecker::checkImplements(ClassDecl *cls) {
  std::vector<const Type *> interfaces;
  interfacesOf(cls, interfaces);
  for (const Type *iface : interfaces) {
    // The interfaces an implemented one extends have their fields in it.
    if (!llvm::is_contained(cls->implemented, iface)) {
      continue;
    }
    std::string prefix = "class '" + cls->name.str() + "' implements '" + iface->name().str() + "'";
    for (const Field &member : iface->fields()) {
      const ClassField *field = ClassBases::findField(cls, member.name);
      if (field && !field->method) {
        const Field *actual = cls->type->field(member.name);
        if (!actual || actual->type->isError() || member.type->isError()) {
          continue;
        }
        // At the field, if the class declares it itself. The class may
        // narrow it to a type of the same references, or make it read-only:
        // code that writes through the interface is found by the check of
        // the whole program (sema_widen.cpp).
        llvm::SMLoc loc = cls->field(member.name) ? field->loc : cls->loc();
        if (actual->type != member.type && !WideWrites::sameReference(actual->type, member.type)) {
          checker_.diags_.error(
              loc, prefix + ", whose field '" + member.name + "' is '" + member.type->name() +
                       "', but in '" + cls->name + "' it is '" + actual->type->name() +
                       "'; the types must be the same, or objects "
                       "of a type down the chain of the interface's, such as 'Dog' for "
                       "'Animal'");
        }
        continue;
      }
      std::vector<FunctionDecl *> methods =
          ClassDeclarer::methodsNamed(cls, member.name, /*isStatic=*/false);
      if (methods.empty()) {
        std::string type = member.type->isOptional() ? member.type->inner()->name().str()
                                                     : member.type->name().str();
        checker_.diags_.error(cls->loc(), prefix + ", but has no '" + member.name +
                                              "'; declare it, e.g. '" + member.name +
                                              (member.type->isOptional() ? "?" : "") + ": " + type +
                                              "'");
        continue;
      }
      if (!member.type->isFunction()) {
        checker_.diags_.error(methods[0]->loc(), "'" + member.name + "' is a field of type '" +
                                                     member.type->name() + "' in '" +
                                                     iface->name() + "', which '" + cls->name +
                                                     "' implements, so it cannot be a method");
        continue;
      }
      if (!member.readonly && llvm::none_of(methods, isArrowMethod)) {
        checker_.diags_.error(methods[0]->loc(),
                              "'" + member.name + "' is a field in '" + iface->name() +
                                  "' that code may replace, so in '" + cls->name +
                                  "', which implements it, it must be a field too: '" +
                                  member.name + " = (...) => ...'; or declare it in '" +
                                  iface->name() + "' as a method, '" + member.name + "(...): ...'");
        continue;
      }
      bool found = false;
      for (FunctionDecl *fn : methods) {
        if (!fn->sym || !takesParams(member.type, fn->sym)) {
          continue;
        }
        found = true;
        const Type *ret = checker_.returnInferrer().returnType(fn->sym);
        if (ret && !ret->isError() && ret != member.type->ret()) {
          checker_.diags_.error(fn->loc(), "'" + fn->name + "' returns '" + ret->name() +
                                               "', and '" + iface->name() + "', which '" +
                                               cls->name + "' implements, says it returns '" +
                                               member.type->ret()->name() + "'");
        }
      }
      if (!found) {
        checker_.diags_.error(methods[0]->loc(),
                              prefix + ", whose '" + member.name + "' is '" + member.type->name() +
                                  "', but '" + cls->name + "." + member.name +
                                  "' takes other parameters: " + variantList(methods));
      }
    }
    // Each signature of a method with several, by a variant of its own.
    for (const Overload &overload : iface->overloads()) {
      std::vector<FunctionDecl *> methods =
          ClassDeclarer::methodsNamed(cls, overload.name, /*isStatic=*/false);
      for (const Type *signature : overload.signatures) {
        auto fn = llvm::find_if(methods, [&](const FunctionDecl *m) {
          return m->sym && takesParams(signature, m->sym);
        });
        if (fn == methods.end()) {
          checker_.diags_.error(cls->loc(),
                                prefix + ", but has no variant of '" + overload.name +
                                    "' for its signature '" + signature->name() +
                                    "'; each signature needs a variant with these parameters");
          continue;
        }
        const Type *ret = checker_.returnInferrer().returnType((*fn)->sym);
        if (ret && !ret->isError() && ret != signature->ret()) {
          checker_.diags_.error((*fn)->loc(), "'" + overload.name + "' returns '" + ret->name() +
                                                  "', and '" + iface->name() + "', which '" +
                                                  cls->name + "' implements, says it returns '" +
                                                  signature->ret()->name() + "'");
        }
      }
    }
  }
}

// `super(args)`: the constructor of the class extended, chosen by the
// arguments among its variants (or those of the nearest class up the chain
// that has any). Then the initializers of this class's fields run, as in
// JavaScript, and `this` may be used.
Expr *InheritChecker::checkSuperCall(CallExpr *call) {
  call->type = Type::voidTy();
  call->callee->type = Type::voidTy();
  call->superCall = true;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  FunctionDecl *ctor = checker_.currentDecl();
  if (!ctor || !ctor->isConstructor || !ctor->cls) {
    checker_.diags_.error(call->loc(),
                          "'super(...)' can be called only in the constructor of a class that "
                          "extends another");
    checkArgsLoosely();
    return call;
  }
  ClassDecl *cls = ctor->cls;
  if (!cls->parent) {
    if (!cls->base) { // with an error in `extends`, it was reported there
      checker_.diags_.error(call->loc(), "'super(...)' calls the constructor of the class that '" +
                                             cls->name + "' extends, and it extends none");
    }
    checkArgsLoosely();
    return call;
  }
  if (!checker_.rootSupers_.contains(call)) {
    checker_.diags_.error(call->loc(),
                          "'super(...)' must be a statement of its own in the body of the "
                          "constructor, not inside a block, a condition, a loop or another "
                          "expression: the fields are set right after it");
    checkArgsLoosely();
    return call;
  }
  if (!checker_.flow_.beforeSuper) {
    checker_.diags_.error(call->loc(),
                          "'super(...)' is called a second time; the constructor of '" +
                              cls->parent->name + "' runs once");
    checkArgsLoosely();
    return call;
  }
  std::vector<FunctionDecl *> ctors = ClassLinks::constructorsOf(cls->parent);
  if (ctors.empty()) {
    if (!call->args.empty()) {
      checker_.diags_.error(call->loc(), "class '" + cls->parent->name +
                                             "' has no constructor, so "
                                             "'super()' takes no arguments");
    }
    checkArgsLoosely();
  } else {
    checker_.classMemberChecker().checkAccess(ctors[0]->cls, ctors[0]->access, "constructor",
                                              call->loc());
    bool checked = false;
    if (FuncSymbol *sym = checker_.overloadResolver().chooseVariant(ctors, call->args, call->spread,
                                                                    call->loc(), checked)) {
      checker_.returnInferrer().resolveParams(sym);
      call->method = sym;
      std::vector<bool> given(checked ? call->args.size() : 0, true);
      checker_.argumentChecker().checkArguments(call, sym->params, sym->minArgs, sym->hasRest,
                                                call->loc(), &given);
    }
  }
  // The fields up the chain are set now, and so are those with initializers.
  checker_.flow_.beforeSuper = false;
  for (const ClassField &f : cls->fields) {
    if (!f.isStatic && ((f.init && !f.method) || f.redeclared) &&
        !llvm::is_contained(checker_.flow_.setFields, f.name)) {
      checker_.flow_.setFields.push_back(f.name);
    }
  }
  checker_.flowChecker().calledFunction(); // the constructors run

  return call;
}

// `super.m(args)`: the method m of the class extended, called directly on
// `this`, whatever the classes down the chain override.
Expr *InheritChecker::checkSuperMethodCall(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  member->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  // An arrow sees the `super` of the method around it.
  FunctionDecl *method = nullptr;
  for (FunctionDecl *fn : llvm::reverse(checker_.functions_)) {
    if (fn->isArrow) {
      continue;
    }
    if (fn->takesThis() && !fn->cls->literal) {
      method = fn;
    }
    break;
  }
  if (!method || !method->cls->parent) {
    if (!method || !method->cls->base) {
      checker_.diags_.error(member->object->loc(),
                            "'super' is the class that the class of the method "
                            "extends; it exists in the methods of a class that "
                            "extends another");
    }
    checkArgsLoosely();
    return call;
  }
  ClassDecl *parent = method->cls->parent;
  // The object is `this`, with its rules while the constructor sets fields.
  auto *self = checker_.ctx_.make<NameExpr>(member->object->loc(), "this");
  checker_.thisMember_ = member->name;
  member->object = checker_.classChecker().checkThis(self);
  checker_.thisMember_ = {};
  if (member->object->type->isError()) {
    checkArgsLoosely();
    return call;
  }
  if (checker_.classChecker().isInitThis(member->object) && !checker_.flow_.beforeSuper &&
      !checker_.classChecker().thisReady(checker_.flow_)) {
    checker_.diags_.error(member->loc(), "'super." + member->name +
                                             "()' is called before every field is set: field '" +
                                             checker_.classChecker().firstUnset(checker_.flow_) +
                                             "' is not set yet, and the method may read it");
  }
  std::vector<FunctionDecl *> methods =
      ClassDeclarer::methodsNamed(parent, member->name, /*isStatic=*/false);
  if (methods.empty()) {
    if (ClassBases::findField(parent, member->name)) {
      checker_.diags_.error(member->loc(), "'" + member->name + "' is a field of '" + parent->name +
                                               "'; call the function it holds as 'this." +
                                               member->name + "(...)'");
    } else {
      checker_.diags_.error(member->loc(),
                            "class '" + parent->name + "' has no method '" + member->name + "'");
    }
    checkArgsLoosely();
    return call;
  }
  checker_.classMemberChecker().checkAccess(methods[0]->cls, methods[0]->access, member->name,
                                            member->loc());
  // `super.on(...)` of an emitter, checked against its event map.
  if (checker_.eventMaps().isEmitterClass(parent)) {
    if (Expr *checked =
            checker_.emitterChecker().checkEmitterCall(call, member, parent, /*super=*/true)) {
      return checked;
    }
  }
  bool checked = false;
  std::vector<const Type *> bound;
  FuncSymbol *sym = checker_.overloadResolver().chooseVariant(methods, call->args, call->spread,
                                                              member->loc(), checked, &bound);
  if (!sym) {
    return call;
  }
  if (sym->decl->isAbstract) {
    checker_.diags_.error(member->loc(), "'" + member->name + "' is abstract in '" +
                                             sym->decl->cls->name + "': it has no body to call");
    checkArgsLoosely();
    return call;
  }
  checker_.returnInferrer().resolveParams(sym);
  call->superCall = true;
  // A generic method of the class extended: the copy for these types.
  if (sym->isGeneric() && checked) {
    return checker_.overloadResolver().callGenericVariant(call, sym, bound, member->loc(),
                                                          /*method=*/true);
  }
  if (sym->isGeneric()) {
    checker_.typeArgInferrer().checkGenericCall(call, sym, member->loc(), nullptr,
                                                member->typeArgs);
    call->method = call->func;
    call->func = nullptr;
    if (call->method) {
      member->type = checker_.returnInferrer().functionType(call->method);
    }
    return call;
  }
  call->method = sym;
  member->type = checker_.returnInferrer().functionType(sym);
  call->type = checker_.returnInferrer().returnType(sym);
  std::vector<bool> given(checked ? call->args.size() : 0, true);

  return checker_.argumentChecker().checkArguments(call, sym->params, sym->minArgs, sym->hasRest,
                                                   member->loc(), &given);
}

} // namespace tz
