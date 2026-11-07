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


namespace tz {

bool InheritChecker::takesParams(const Type *fnType, const FuncSymbol *sym) {
  return fnType->isFunction() && fnType->params() == llvm::ArrayRef<const Type *>(sym->params) &&
         fnType->minArgs() == sym->minArgs && fnType->hasRest() == sym->hasRest;
}

bool InheritChecker::isArrowMethod(const FunctionDecl *fn) {
  if (!fn->cls()) {
    return false;
  }
  for (const ClassField &f : fn->cls()->fields) {
    if (f.method() == fn) {
      return true;
    }
  }

  return false;
}

void InheritChecker::interfacesOf(const ClassDecl *cls, std::vector<const Type *> &out) {
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

InheritChecker::InheritChecker(Checker &owner)
    : checker(owner) {}

InheritChecker::ResultAbstractHasClass InheritChecker::abstractNeedsClass(const ClassDecl *cls,
                                                                          const FunctionDecl *fn) {
  if (fn->isAbstract() && !cls->isAbstract) {
    std::string message = ErrorTextAbstract::needsClass(fn->name, cls->name);

    return err(InheritCheckerAbstractNeedsClassError{
        .code = InheritCheckerAbstractNeedsClassErrorCode::ABSTRACT_IN_CONCRETE,
        .message = message,
    });
  }

  return ok(fn);
}

InheritChecker::ResultAbstractOverride
InheritChecker::abstractImplemented(const ClassDecl *cls, const FunctionDecl *found) {
  if (found->cls() != cls) {
    std::string variant = variantText(found);
    std::string message = ErrorTextAbstract::notImpl(cls->name, variant, found->cls()->name);

    return err(InheritCheckerAbstractImplementedError{
        .code = InheritCheckerAbstractImplementedErrorCode::NOT_IMPLEMENTED,
        .message = message,
    });
  }

  return ok(found);
}

void InheritChecker::checkAbstract(ClassDecl *cls) {
  if (cls->literal()) {
    return;
  }
  for (FunctionDecl *fn : ClassLinks::ownMethods(cls)) {
    ResultAbstractHasClass abstract = InheritChecker::abstractNeedsClass(cls, fn);
    if (abstract.isErr()) {
      this->checker.diags_.error(fn->loc(), abstract.error().message);
    }
  }
  if (cls->isAbstract) {
    return;
  }
  for (size_t slot = 0; slot < cls->vtable.size(); ++slot) {
    if (cls->vtable[slot]) {
      continue;
    }
    for (const ClassDecl *c = cls; c; c = c->parent) {
      FunctionDecl *found = nullptr;
      for (FunctionDecl *fn : ClassLinks::ownMethods(c)) {
        if (fn->slot() == int(slot)) {
          found = fn;
        }
      }
      if (!found) {
        continue;
      }
      ResultAbstractOverride implemented = InheritChecker::abstractImplemented(cls, found);
      if (implemented.isErr()) {
        this->checker.diags_.error(cls->loc(), implemented.error().message);
      }
      break;
    }
  }
}

InheritChecker::ResultFieldTypeMatches InheritChecker::fieldMatches(llvm::StringRef prefix,
                                                                    const Field &member,
                                                                    const Field *actual,
                                                                    const ClassDecl *cls) {
  bool same = actual->type == member.type || WideWrites::sameReference(actual->type, member.type);
  if (!same) {
    std::string message = ErrorTextImplements::fieldType(prefix, member.name, member.type->name(),
                                                         cls->name, actual->type->name());

    return err(InheritCheckerFieldMatchesError{
        .code = InheritCheckerFieldMatchesErrorCode::FIELD_TYPE_DIFFERS,
        .message = message,
    });
  }

  return ok(actual);
}

InheritChecker::ResultMemberIsDeclared
InheritChecker::memberDeclared(llvm::StringRef prefix, const Field &member,
                               llvm::ArrayRef<FunctionDecl *> methods) {
  if (methods.empty()) {
    bool optional = member.type->isOptional();
    std::string type = optional ? member.type->inner()->name().str() : member.type->name().str();
    std::string message = ErrorTextImplements::noMember(prefix, member.name, optional, type);

    return err(InheritCheckerMemberDeclaredError{
        .code = InheritCheckerMemberDeclaredErrorCode::NO_MEMBER,
        .message = message,
    });
  }

  return ok(&member);
}

InheritChecker::ResultMemberIsFunction
InheritChecker::methodForField(const Field &member, const Type *iface, const ClassDecl *cls) {
  if (!member.type->isFunction()) {
    std::string message =
        ErrorTextImplements::fieldNotFn(member.name, member.type->name(), iface->name(), cls->name);

    return err(InheritCheckerMethodForFieldError{
        .code = InheritCheckerMethodForFieldErrorCode::FIELD_AS_METHOD,
        .message = message,
    });
  }

  return ok(&member);
}

InheritChecker::ResultReplaceableField
InheritChecker::replaceable(const Field &member, llvm::ArrayRef<FunctionDecl *> methods,
                            const Type *iface, const ClassDecl *cls) {
  if (!member.readonly && llvm::none_of(methods, InheritChecker::isArrowMethod)) {
    std::string message = ErrorTextImplements::mustField(member.name, iface->name(), cls->name);

    return err(InheritCheckerReplaceableError{
        .code = InheritCheckerReplaceableErrorCode::METHOD_NOT_FIELD,
        .message = message,
    });
  }

  return ok(&member);
}

InheritChecker::ResultReturnTypeAgrees
InheritChecker::returnMatches(llvm::StringRef name, const Type *ret, const Type *expected,
                              const Type *iface, const ClassDecl *cls) {
  if (ret && !ret->isError() && ret != expected) {
    std::string message =
        ErrorTextImplements::returns(name, ret->name(), iface->name(), cls->name, expected->name());

    return err(InheritCheckerReturnMatchesError{
        .code = InheritCheckerReturnMatchesErrorCode::RETURN_DIFFERS,
        .message = message,
    });
  }

  return ok(ret);
}

InheritChecker::ResultVariantForParams
InheritChecker::variantFound(llvm::StringRef prefix, const Field &member,
                             llvm::ArrayRef<FunctionDecl *> methods, const ClassDecl *cls,
                             bool found) {
  if (!found) {
    std::string variants = variantList(methods);
    std::string message =
        ErrorTextImplements::params(prefix, member.name, member.type->name(), cls->name, variants);

    return err(InheritCheckerVariantFoundError{
        .code = InheritCheckerVariantFoundErrorCode::OTHER_PARAMETERS,
        .message = message,
    });
  }

  return ok(&member);
}

InheritChecker::ResultSignatureVariant InheritChecker::signatureVariant(llvm::StringRef prefix,
                                                                        llvm::StringRef name,
                                                                        const Type *signature,
                                                                        bool found) {
  if (!found) {
    std::string message = ErrorTextImplements::noVariant(prefix, name, signature->name());

    return err(InheritCheckerSignatureVariantError{
        .code = InheritCheckerSignatureVariantErrorCode::NO_VARIANT,
        .message = message,
    });
  }

  return ok(signature);
}

void InheritChecker::checkImplements(ClassDecl *cls) {
  std::vector<const Type *> interfaces;
  InheritChecker::interfacesOf(cls, interfaces);
  for (const Type *iface : interfaces) {
    if (!llvm::is_contained(cls->implemented, iface)) {
      continue;
    }
    std::string prefix = ErrorTextImplements::prefix(cls->name, iface->name());
    for (const Field &member : iface->fields()) {
      const ClassField *field = ClassBases::findField(cls, member.name);
      if (field && !field->method()) {
        const Field *actual = cls->type->field(member.name);
        if (!actual || actual->type->isError() || member.type->isError()) {
          continue;
        }
        llvm::SMLoc loc = cls->field(member.name) ? field->loc : cls->loc();
        ResultFieldTypeMatches matches = InheritChecker::fieldMatches(prefix, member, actual, cls);
        if (matches.isErr()) {
          this->checker.diags_.error(loc, matches.error().message);
        }
        continue;
      }
      std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, member.name, false);
      ResultMemberIsDeclared declared = InheritChecker::memberDeclared(prefix, member, methods);
      if (declared.isErr()) {
        this->checker.diags_.error(cls->loc(), declared.error().message);
        continue;
      }
      ResultMemberIsFunction function = InheritChecker::methodForField(member, iface, cls);
      if (function.isErr()) {
        this->checker.diags_.error(methods[0]->loc(), function.error().message);
        continue;
      }
      ResultReplaceableField replaced = InheritChecker::replaceable(member, methods, iface, cls);
      if (replaced.isErr()) {
        this->checker.diags_.error(methods[0]->loc(), replaced.error().message);
        continue;
      }
      bool found = false;
      for (FunctionDecl *fn : methods) {
        if (!fn->sym || !InheritChecker::takesParams(member.type, fn->sym)) {
          continue;
        }
        found = true;
        const Type *ret = this->checker.returnInferrer().returnType(fn->sym);
        ResultReturnTypeAgrees agrees =
            InheritChecker::returnMatches(fn->name, ret, member.type->ret(), iface, cls);
        if (agrees.isErr()) {
          this->checker.diags_.error(fn->loc(), agrees.error().message);
        }
      }
      ResultVariantForParams variant =
          InheritChecker::variantFound(prefix, member, methods, cls, found);
      if (variant.isErr()) {
        this->checker.diags_.error(methods[0]->loc(), variant.error().message);
      }
    }
    for (const Overload &overload : iface->overloads()) {
      std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, overload.name, false);
      for (const Type *signature : overload.signatures) {
        auto fn = llvm::find_if(methods, [&](const FunctionDecl *m) {
          return m->sym && InheritChecker::takesParams(signature, m->sym);
        });
        ResultSignatureVariant variant =
            InheritChecker::signatureVariant(prefix, overload.name, signature, fn != methods.end());
        if (variant.isErr()) {
          this->checker.diags_.error(cls->loc(), variant.error().message);
          continue;
        }
        const Type *ret = this->checker.returnInferrer().returnType((*fn)->sym);
        ResultReturnTypeAgrees agrees =
            InheritChecker::returnMatches(overload.name, ret, signature->ret(), iface, cls);
        if (agrees.isErr()) {
          this->checker.diags_.error((*fn)->loc(), agrees.error().message);
        }
      }
    }
  }
}

InheritChecker::ResultConstructorScope InheritChecker::superAllowed(FunctionDecl *ctor) {
  if (!ctor || !ctor->isConstructor() || !ctor->cls()) {
    std::string message = ErrorTextSuperCall::outside();

    return err(InheritCheckerSuperAllowedError{
        .code = InheritCheckerSuperAllowedErrorCode::NOT_IN_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok(ctor);
}

InheritChecker::ResultParentClassFound InheritChecker::parentClass(ClassDecl *cls) {
  if (!cls->parent && !cls->base) {
    std::string message = ErrorTextSuperCall::noParent(cls->name);

    return err(InheritCheckerParentClassError{
        .code = InheritCheckerParentClassErrorCode::NO_PARENT,
        .message = message,
    });
  }
  if (!cls->parent) {
    return err(InheritCheckerParentClassError{
        .code = InheritCheckerParentClassErrorCode::BASE_ONLY,
        .message = "",
    });
  }

  return ok(cls->parent);
}

InheritChecker::ResultSuperIsStatement InheritChecker::rootSuper(const CallExpr *call) const {
  if (!this->checker.rootSupers_.contains(call)) {
    std::string message = ErrorTextSuperCall::statement();

    return err(InheritCheckerRootSuperError{
        .code = InheritCheckerRootSuperErrorCode::NOT_A_STATEMENT,
        .message = message,
    });
  }

  return ok(call);
}

InheritChecker::ResultSuperNotRepeated InheritChecker::superOnce(const ClassDecl *cls) const {
  if (!this->checker.flow_.beforeSuper) {
    std::string message = ErrorTextSuperCall::twice(cls->parent->name);

    return err(InheritCheckerSuperOnceError{
        .code = InheritCheckerSuperOnceErrorCode::CALLED_TWICE,
        .message = message,
    });
  }

  return ok(cls);
}

InheritChecker::ResultNoSuperArguments InheritChecker::superArguments(const CallExpr *call,
                                                                      const ClassDecl *parent) {
  if (!call->args.empty()) {
    std::string message = ErrorTextSuperCall::noArgs(parent->name);

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
  ResultConstructorScope scope = InheritChecker::superAllowed(this->checker.currentDecl());
  if (scope.isErr()) {
    this->checker.diags_.error(call->loc(), scope.error().message);
    this->checker.argumentChecker().checkArgsLoosely(call);
    return call;
  }
  ClassDecl *cls = scope.value()->cls();
  ResultParentClassFound parent = InheritChecker::parentClass(cls);
  if (parent.isErr()) {
    if (parent.error().code != InheritCheckerParentClassErrorCode::BASE_ONLY) {
      this->checker.diags_.error(call->loc(), parent.error().message);
    }
    this->checker.argumentChecker().checkArgsLoosely(call);
    return call;
  }
  ResultSuperIsStatement statement = this->rootSuper(call);
  if (statement.isErr()) {
    this->checker.diags_.error(call->loc(), statement.error().message);
    this->checker.argumentChecker().checkArgsLoosely(call);
    return call;
  }
  ResultSuperNotRepeated once = this->superOnce(cls);
  if (once.isErr()) {
    this->checker.diags_.error(call->loc(), once.error().message);
    this->checker.argumentChecker().checkArgsLoosely(call);
    return call;
  }
  std::vector<FunctionDecl *> ctors = ClassLinks::constructorsOf(cls->parent);
  if (ctors.empty()) {
    ResultNoSuperArguments none = InheritChecker::superArguments(call, cls->parent);
    if (none.isErr()) {
      this->checker.diags_.error(call->loc(), none.error().message);
    }
    this->checker.argumentChecker().checkArgsLoosely(call);
  } else {
    bool checked = false;
    OverloadResolver::ResultVariantChosen picked =
        this->checker.overloadResolver().chooseVariant(ctors, call->args, call->spread, checked);
    if (picked.isErr() &&
        picked.error().code != OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED) {
      this->checker.diags_.error(call->loc(), picked.error().message);
    }
    if (picked.isOk()) {
      FuncSymbol *sym = picked.value();
      const FunctionDecl *chosen = sym->decl ? sym->decl : ctors[0];
      ClassMemberChecker::ResultMemberAccessible accessible =
          this->checker.classMemberChecker().checkAccess(chosen->cls(), chosen->access(),
                                                         "constructor");
      if (accessible.isErr()) {
        this->checker.diags_.error(call->loc(), accessible.error().message);
      }
      this->checker.returnInferrer().resolveParams(sym);
      call->target = SuperCall{.method = sym};
      std::vector<bool> given(checked ? call->args.size() : 0, true);
      this->checker.argumentChecker().checkArguments(call, sym->params, sym->minArgs, sym->hasRest,
                                                     call->loc(), &given);
    }
  }
  this->checker.flow_.beforeSuper = false;
  for (const ClassField &f : cls->fields) {
    if (!f.isStatic() && ((f.init && !f.method()) || f.redeclared) &&
        !llvm::is_contained(this->checker.flow_.setFields, f.name)) {
      this->checker.flow_.setFields.push_back(f.name);
    }
  }
  this->checker.flowChecker().calledFunction();

  return call;
}

InheritChecker::ResultSuperMethodScope InheritChecker::superContext(FunctionDecl *method) {
  if (!method || (!method->cls()->parent && !method->cls()->base)) {
    std::string message = ErrorTextSuperMethod::outside();

    return err(InheritCheckerSuperContextError{
        .code = InheritCheckerSuperContextErrorCode::NO_SUPER_CLASS,
        .message = message,
    });
  }
  if (!method->cls()->parent) {
    return err(InheritCheckerSuperContextError{
        .code = InheritCheckerSuperContextErrorCode::BASE_ONLY,
        .message = "",
    });
  }

  return ok(method);
}

InheritChecker::ResultObjectIsComplete
InheritChecker::objectComplete(const MemberExpr *member) const {
  bool early = this->checker.classChecker().isInitThis(member->object).isOk() &&
               !this->checker.flow_.beforeSuper &&
               this->checker.classChecker().thisReady(this->checker.flow_).isErr();
  if (early) {
    std::string field = this->checker.classChecker().firstUnset(this->checker.flow_).value();
    std::string message = ErrorTextSuperMethod::early(member->name, field);

    return err(InheritCheckerObjectCompleteError{
        .code = InheritCheckerObjectCompleteErrorCode::CALLED_BEFORE_SET,
        .message = message,
    });
  }

  return ok(member);
}

InheritChecker::ResultParentHasMethods InheritChecker::parentMethods(const ClassDecl *parent,
                                                                     llvm::StringRef name) {
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(parent, name, false);
  if (methods.empty() && ClassBases::findField(parent, name)) {
    std::string message = ErrorTextSuperMethod::isField(name, parent->name);

    return err(InheritCheckerParentMethodsError{
        .code = InheritCheckerParentMethodsErrorCode::FIELD_OF_PARENT,
        .message = message,
    });
  }
  if (methods.empty()) {
    std::string message = ErrorTextSuperMethod::noMethod(parent->name, name);

    return err(InheritCheckerParentMethodsError{
        .code = InheritCheckerParentMethodsErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  return ok(methods);
}

InheritChecker::ResultMethodHasItsBody InheritChecker::methodBody(FuncSymbol *sym,
                                                                  llvm::StringRef name) {
  if (sym->decl->isAbstract()) {
    std::string message = ErrorTextSuperMethod::abstract(name, sym->decl->cls()->name);

    return err(InheritCheckerMethodBodyError{
        .code = InheritCheckerMethodBodyErrorCode::ABSTRACT_METHOD,
        .message = message,
    });
  }

  return ok(sym);
}

Expr *InheritChecker::checkSuperMethodCall(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  member->type = Type::error();
  FunctionDecl *method = nullptr;
  for (FunctionDecl *fn : llvm::reverse(this->checker.functions_)) {
    if (fn->isArrow()) {
      continue;
    }
    if (fn->takesThis() && !fn->cls()->literal()) {
      method = fn;
    }
    break;
  }
  ResultSuperMethodScope scope = InheritChecker::superContext(method);
  if (scope.isErr()) {
    if (scope.error().code != InheritCheckerSuperContextErrorCode::BASE_ONLY) {
      this->checker.diags_.error(member->object->loc(), scope.error().message);
    }
    this->checker.argumentChecker().checkArgsLoosely(call);
    return call;
  }
  ClassDecl *parent = method->cls()->parent;
  auto *self = this->checker.ctx_.make<NameExpr>(member->object->loc(), "this");
  this->checker.thisMember_ = member->name;
  ClassChecker::ResultThisReference reference = this->checker.classChecker().checkThis(self);
  this->checker.thisMember_ = {};
  if (reference.isErr()) {
    this->checker.diags_.error(self->loc(), reference.error().message);
  }
  member->object = reference.isOk() ? reference.value() : self;
  if (member->object->type->isError()) {
    this->checker.argumentChecker().checkArgsLoosely(call);
    return call;
  }
  ResultObjectIsComplete complete = this->objectComplete(member);
  if (complete.isErr()) {
    this->checker.diags_.error(member->loc(), complete.error().message);
  }
  ResultParentHasMethods found = InheritChecker::parentMethods(parent, member->name);
  if (found.isErr()) {
    this->checker.diags_.error(member->loc(), found.error().message);
    this->checker.argumentChecker().checkArgsLoosely(call);
    return call;
  }
  std::vector<FunctionDecl *> methods = found.value();
  ClassMemberChecker::ResultMemberAccessible accessible =
      this->checker.classMemberChecker().checkAccess(methods[0]->cls(), methods[0]->access(),
                                                     member->name);
  if (accessible.isErr()) {
    this->checker.diags_.error(member->loc(), accessible.error().message);
  }
  if (this->checker.eventMaps().isEmitterClass(parent)) {
    if (Expr *checked =
            this->checker.emitterChecker().checkEmitterCall(call, member, parent, true)) {
      return checked;
    }
  }
  bool checked = false;
  std::vector<const Type *> bound;
  OverloadResolver::ResultVariantChosen picked = this->checker.overloadResolver().chooseVariant(
      methods, call->args, call->spread, checked, &bound);
  if (picked.isErr() &&
      picked.error().code != OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED) {
    this->checker.diags_.error(member->loc(), picked.error().message);
  }
  if (picked.isErr()) {
    return call;
  }
  ResultMethodHasItsBody body = InheritChecker::methodBody(picked.value(), member->name);
  if (body.isErr()) {
    this->checker.diags_.error(member->loc(), body.error().message);
    this->checker.argumentChecker().checkArgsLoosely(call);
    return call;
  }
  FuncSymbol *sym = body.value();
  this->checker.returnInferrer().resolveParams(sym);
  call->target = SuperCall{};
  if (sym->isGeneric() && checked) {
    return this->checker.overloadResolver().callGenericVariant(call, sym, bound, member->loc(),
                                                               true);
  }
  if (sym->isGeneric()) {
    this->checker.typeArgInferrer().checkGenericCall(call, sym, member->loc(), nullptr,
                                                     member->typeArgs);
    call->target = SuperCall{.method = call->func()};
    if (call->method()) {
      member->type = this->checker.returnInferrer().functionType(call->method());
    }
    return call;
  }
  call->target = SuperCall{.method = sym};
  member->type = this->checker.returnInferrer().functionType(sym);
  call->type = this->checker.returnInferrer().returnType(sym);
  std::vector<bool> given(checked ? call->args.size() : 0, true);

  return this->checker.argumentChecker().checkArguments(call, sym->params, sym->minArgs,
                                                        sym->hasRest, member->loc(), &given);
}

}
