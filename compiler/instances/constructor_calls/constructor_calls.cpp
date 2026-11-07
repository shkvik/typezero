#include "compiler/instances/constructor_calls/constructor_calls.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

ConstructorCalls::ConstructorCalls(Checker &owner)
    : checker(owner) {}

void ConstructorCalls::checkConstructorCalls() {
  for (ClassDecl *cls : this->checker.classes_) {
    bool skipped = cls->literal() || cls->children.empty() || cls->generic();
    if (skipped) {
      continue;
    }
    std::vector<FunctionDecl *> functions = cls->constructors;
    if (cls->init) {
      functions.push_back(cls->init);
    }
    for (FunctionDecl *ctor : functions) {
      llvm::SmallPtrSet<const FunctionDecl *, 8> seen;
      this->scanConstructorCalls(ctor, cls, nullptr, ctor, seen);
    }
  }
}

const NameExpr *ConstructorCalls::receiverName(const CallExpr *call) {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  const Expr *object = member ? member->object : nullptr;
  while (auto *conv = llvm::dyn_cast_or_null<CastExpr>(object)) {
    object = conv->operand;
  }

  return llvm::dyn_cast_or_null<NameExpr>(object);
}

const ClassDecl *ConstructorCalls::overridingChild(const ClassDecl *cls,
                                                   const FunctionDecl *method) {
  for (const ClassDecl *child : cls->children) {
    for (FunctionDecl *other : ClassLinks::ownMethods(child)) {
      if (ClassLinks::overridesTransitively(other, method)) {
        return child;
      }
    }
    const ClassDecl *found = ConstructorCalls::overridingChild(child, method);
    if (found) {
      return found;
    }
  }

  return nullptr;
}

ConstructorCalls::ResultNotOverridden ConstructorCalls::overriddenCall(const ClassDecl *cls,
                                                                       const FunctionDecl *ctor,
                                                                       const CallExpr *call,
                                                                       const CallExpr *site) {
  FunctionDecl *target = call->method()->decl;
  bool overridable = call->virtualCall() || !cls->derivesFrom(target->cls());
  const ClassDecl *child =
      call->virtualCall() ? ConstructorCalls::overridingChild(cls, target) : nullptr;
  const ClassDecl *by = child ? child : target->cls();
  bool direct = site == call;
  if (overridable && ctor->isConstructor() && direct) {
    std::string message = ErrorTextOverriddenCall::ctorCall(cls->name, target->name, by->name);

    return err(ConstructorCallsOverriddenCallError{
        .code = ConstructorCallsOverriddenCallErrorCode::CONSTRUCTOR_CALLS_OVERRIDE,
        .message = message,
    });
  }
  if (overridable && ctor->isConstructor()) {
    std::string message = ErrorTextOverriddenCall::ctorReach(cls->name, target->name, by->name);

    return err(ConstructorCallsOverriddenCallError{
        .code = ConstructorCallsOverriddenCallErrorCode::CONSTRUCTOR_REACHES_OVERRIDE,
        .message = message,
    });
  }
  if (overridable && direct) {
    std::string message = ErrorTextOverriddenCall::fieldsCall(cls->name, target->name, by->name);

    return err(ConstructorCallsOverriddenCallError{
        .code = ConstructorCallsOverriddenCallErrorCode::INITIALIZERS_CALL_OVERRIDE,
        .message = message,
    });
  }
  if (overridable) {
    std::string message = ErrorTextOverriddenCall::fieldsReach(cls->name, target->name, by->name);

    return err(ConstructorCallsOverriddenCallError{
        .code = ConstructorCallsOverriddenCallErrorCode::INITIALIZERS_REACH_OVERRIDE,
        .message = message,
    });
  }

  return ok(target);
}

void ConstructorCalls::scanConstructorCalls(const FunctionDecl *fn, const ClassDecl *cls,
                                            const CallExpr *origin, const FunctionDecl *ctor,
                                            llvm::SmallPtrSetImpl<const FunctionDecl *> &seen) {
  if (!fn->body || !fn->thisVar()) {
    return;
  }
  bool firstVisit = seen.insert(fn).second;
  if (!firstVisit) {
    return;
  }

  const VarSymbol *self = fn->thisVar();
  auto scanCall = [&](const Expr *e) {
    auto *call = dyn_cast<CallExpr>(e);
    bool originReported = origin && this->reportedCalls.contains(origin);
    if (!call || !call->method() || originReported) {
      return;
    }
    const NameExpr *name = ConstructorCalls::receiverName(call);
    if (!name || name->var() != self) {
      return;
    }
    const CallExpr *site = origin ? origin : call;
    ResultNotOverridden target = ConstructorCalls::overriddenCall(cls, ctor, call, site);
    if (target.isErr()) {
      this->checker.diags_.error(site->loc(), target.error().message);
      this->reportedCalls.insert(site);
      return;
    }
    if (!origin) {
      seen.clear();
    }
    seen.insert(fn);
    this->scanConstructorCalls(target.value(), cls, site, ctor, seen);
  };
  visitExprs(fn->body, scanCall, false);
}

}
