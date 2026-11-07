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

namespace tz {

ClassField *StaticChecker::staticField(ClassDecl *cls, llvm::StringRef name, ClassDecl **owner) {
  for (ClassDecl *c = cls; c; c = c->parent) {
    for (ClassField &f : c->staticOwner()->fields) {
      if (f.isStatic() && f.name == name && f.var()) {
        *owner = c->staticOwner();
        return &f;
      }
    }
  }

  return nullptr;
}

StaticChecker::StaticChecker(Checker &owner)
    : checker(owner) {}

Expr *StaticChecker::checkStaticMember(MemberExpr *member, ClassDecl *cls, const Type *expected) {
  llvm::StringRef name = member->name;
  member->type = Type::error();
  ClassDecl *owner = nullptr;
  if (ClassField *found = StaticChecker::staticField(cls, name, &owner)) {
    ClassField &f = *found;
    this->checker.classMemberChecker().checkAccess(owner, f.access, name, member->loc());
    auto *read = this->checker.ctx_.make<NameExpr>(member->loc(), f.var()->name);
    read->binding = NamedVar{.var = f.var()};
    if (member->object && member->object->loc() != member->loc()) {
      this->checker.file_->namedUses[read] = {cls, member->object->loc(), ""};
    }
    if (!f.var()->type) {
      if (!owner->declared && this->checker.loading_ && this->checker.functions_.empty()) {
        this->checker.diags_.error(member->loc(), "'" + f.var()->name + "' is used before class '" +
                                                      owner->name + "' is declared, which sets it");
      } else {
        this->checker.diags_.error(member->loc(),
                                   "the type of '" + f.var()->name +
                                       "' comes from its value, which is set later; declare "
                                       "it: 'static " +
                                       name + ": T = ...'");
      }
      read->type = Type::error();
      return read;
    }
    if (!owner->declared && this->checker.loading_ && this->checker.functions_.empty() &&
        this->checker.declModules_.lookup(owner) == this->checker.loading_) {
      this->checker.diags_.error(member->loc(), "'" + f.var()->name + "' is used before class '" +
                                                    owner->name + "' is declared, which sets it");
    } else if (this->checker.staticInit_ && this->checker.currentClass_ == owner &&
               !this->checker.staticsSet_.contains(f.var())) {
      this->checker.diags_.error(member->loc(),
                                 "'" + f.var()->name +
                                     "' is read before it is set: the static "
                                     "fields are set in the order they are declared");
    }
    read->type = f.var()->type;
    if (read->type->isOptional() && this->checker.isDefined(f.var())) {
      return this->checker.typeRelations().convert(read, read->type->inner());
    }
    return read;
  }
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, true);
  if (methods.empty() && (name == "name" || name == "prototype")) {
    auto *object = cast<NameExpr>(member->object);
    member->object = this->checker.classValueChecker().checkClassValue(object, cls);
    return this->checker.classValueChecker().checkClassRefMember(member);
  }
  if (methods.empty()) {
    if (ClassBases::findField(cls, name) ||
        !ClassDeclarer::methodsNamed(cls, name, false).empty()) {
      this->checker.diags_.error(
          member->loc(), "'" + name + "' is a member of the instances of '" + cls->name +
                             "', not of the class: create one with 'new " + cls->name + "(...)'");
    } else {
      this->checker.diags_.error(member->loc(),
                                 "class '" + cls->name + "' has no static member '" + name + "'");
    }
    return member;
  }
  this->checker.classMemberChecker().checkAccess(methods[0]->cls(), methods[0]->access(), name,
                                                 member->loc());
  FuncSymbol *sym = this->checker.overloadResolver().variantForType(methods, expected, member);
  if (!sym) {
    return member;
  }
  auto *value = this->checker.ctx_.make<NameExpr>(member->loc(), name);
  value->binding = NamedFunction{.func = sym};
  value->type = this->checker.returnInferrer().functionType(sym);

  return value;
}

const Type *StaticChecker::checkStaticTarget(Expr *&target, ClassDecl *cls) {
  auto *member = cast<MemberExpr>(target);
  member->object->type = member->type = Type::error();
  ClassDecl *owner = nullptr;
  if (ClassField *found = StaticChecker::staticField(cls, member->name, &owner)) {
    ClassField &f = *found;
    this->checker.classMemberChecker().checkAccess(owner, f.access, f.name, member->loc());
    auto *name = this->checker.ctx_.make<NameExpr>(member->loc(), f.var()->name);
    name->binding = NamedVar{.var = f.var()};
    name->type = f.var()->type ? f.var()->type : Type::error();
    target = name;
    if (f.var()->isConst) {
      this->checker.diags_.error(member->loc(), "cannot assign to '" + f.var()->name +
                                                    "' because it is a read-only static field");
    }
    f.var()->reassigned = true;
    if (!f.var()->type) {
      this->checker.diags_.error(member->loc(),
                                 "the type of '" + f.var()->name +
                                     "' comes from its value, which is set later; declare it: "
                                     "'static " +
                                     f.name + ": T = ...'");
      return Type::error();
    }
    return f.var()->type;
  }
  if (!ClassDeclarer::methodsNamed(cls, member->name, true).empty()) {
    this->checker.diags_.error(member->loc(), "'" + member->name + "' is a static method of '" +
                                                  cls->name + "' and cannot be replaced");
  } else {
    this->checker.diags_.error(member->loc(), "class '" + cls->name + "' has no static field '" +
                                                  member->name + "'");
  }

  return Type::error();
}

Expr *StaticChecker::checkClassStaticCall(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                                          const Type *expected) {
  call->type = Type::error();
  static const llvm::StringLiteral eventsFunctions[] = {
      "once", "on", "listenerCount", "getEventListeners"};
  if (cls->library() && cls->name == "EventEmitter" &&
      llvm::is_contained(eventsFunctions, member->name)) {
    if (Expr *events = this->checker.eventTargetChecker().checkEventsFunction(call, member->name,
                                                                              member->loc())) {
      return events;
    }
    return this->checker.callChecker().checkDirectCall(
        call, this->checker.libFunctions_.lookup(member->name)->sym, member->loc(), expected);
  }
  if (cls->library() && cls->name == "Readable" && member->name == "from") {
    return this->checker.streamSources().checkStreamFrom(call, member);
  }
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, member->name, true);
  if (methods.empty()) {
    call->callee = this->checkStaticMember(member, cls, nullptr);
    const Type *type = call->callee->type;
    if (type->isFunction()) {
      return this->checker.callChecker().checkValueCall(call, type);
    }
    if (!type->isError()) {
      this->checker.diags_.error(member->loc(), "'" + cls->name + "." + member->name +
                                                    "' is not a function; it has type '" +
                                                    type->name() + "'");
    }
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
    return call;
  }
  this->checker.classMemberChecker().checkAccess(methods[0]->cls(), methods[0]->access(),
                                                 member->name, member->loc());
  bool checked = false;
  std::vector<const Type *> bound;
  FuncSymbol *sym = this->checker.overloadResolver().chooseVariant(
      methods, call->args, call->spread, member->loc(), checked, &bound);
  if (!sym) {
    return call;
  }
  this->checker.returnInferrer().resolveParams(sym);
  if (!sym->isGeneric() && !member->typeArgs.empty()) {
    this->checker.diags_.error(
        member->loc(), "'" + member->name + "' is not a generic method; remove the type arguments");
  }
  if (sym->isGeneric() && checked) {
    return this->checker.overloadResolver().callGenericVariant(call, sym, bound, member->loc(),
                                                               false);
  }
  if (sym->isGeneric()) {
    return this->checker.typeArgInferrer().checkGenericCall(call, sym, member->loc(), expected,
                                                            member->typeArgs);
  }
  call->target = FunctionCall{.func = sym};
  call->type = this->checker.returnInferrer().returnType(sym);
  std::vector<bool> given(checked ? call->args.size() : 0, true);

  return this->checker.argumentChecker().checkArguments(call, sym->params, sym->minArgs,
                                                        sym->hasRest, member->loc(), &given);
}

}
