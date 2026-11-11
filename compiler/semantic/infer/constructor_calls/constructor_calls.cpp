#include "compiler/semantic/infer/constructor_calls/constructor_calls.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/declare/class_tree/class_tree.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

ConstructorCalls::ConstructorCalls(SemanticState &owner)
    : state(owner) {}

void ConstructorCalls::walkConstructorCalls() {
  for (ClassDecl *cls : this->state.getClassStore().getClasses()) {
    bool isSkipped = cls->literal() || cls->children.empty() || cls->generic();
    if (isSkipped) {
      continue;
    }
    std::vector<FunctionDecl *> functions = cls->constructors;
    if (cls->init) {
      functions.push_back(cls->init);
    }
    for (FunctionDecl *ctor : functions) {
      this->scanConstructorCalls(ctor, cls);
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
  for (const ClassDecl *child : ClassTree::descendants(cls)) {
    for (FunctionDecl *other : ClassLinks::ownMethods(child)) {
      if (ClassLinks::overridesTransitively(other, method)) {
        return child;
      }
    }
  }

  return nullptr;
}

ConstructorCalls::ResultNotOverridden ConstructorCalls::overriddenCall(const ClassDecl *cls,
                                                                       const FunctionDecl *ctor,
                                                                       const CallExpr *call,
                                                                       const CallExpr *site) {
  FunctionDecl *target = call->method()->decl;
  bool isOverridable = call->virtualCall() || !cls->derivesFrom(target->cls());
  const ClassDecl *child =
      call->virtualCall() ? ConstructorCalls::overridingChild(cls, target) : nullptr;
  const ClassDecl *by = child ? child : target->cls();
  bool isDirect = site == call;
  if (isOverridable && ctor->isConstructor() && isDirect) {
    std::string message = ErrorTextOverriddenCall::ctorCall(cls->name, target->name, by->name);

    return err(ConstructorCallsOverriddenCallError{
        .code = ConstructorCallsOverriddenCallErrorCode::CONSTRUCTOR_CALLS_OVERRIDE,
        .message = message,
    });
  }
  if (isOverridable && ctor->isConstructor()) {
    std::string message = ErrorTextOverriddenCall::ctorReach(cls->name, target->name, by->name);

    return err(ConstructorCallsOverriddenCallError{
        .code = ConstructorCallsOverriddenCallErrorCode::CONSTRUCTOR_REACHES_OVERRIDE,
        .message = message,
    });
  }
  if (isOverridable && isDirect) {
    std::string message = ErrorTextOverriddenCall::fieldsCall(cls->name, target->name, by->name);

    return err(ConstructorCallsOverriddenCallError{
        .code = ConstructorCallsOverriddenCallErrorCode::INITIALIZERS_CALL_OVERRIDE,
        .message = message,
    });
  }
  if (isOverridable) {
    std::string message = ErrorTextOverriddenCall::fieldsReach(cls->name, target->name, by->name);

    return err(ConstructorCallsOverriddenCallError{
        .code = ConstructorCallsOverriddenCallErrorCode::INITIALIZERS_REACH_OVERRIDE,
        .message = message,
    });
  }

  return ok(target);
}

void ConstructorCalls::scanConstructorCalls(const FunctionDecl *ctor, const ClassDecl *cls) {
  llvm::SmallPtrSet<const FunctionDecl *, 8> seen;
  std::vector<ScanFrame> frames;
  ConstructorCalls::enterScan(frames, ctor, nullptr, seen);
  while (!frames.empty()) {
    std::optional<ScanTarget> next = this->takeNextTarget(frames.back(), cls, ctor, seen);
    if (next.has_value()) {
      ConstructorCalls::enterScan(frames, next->fn, next->site, seen);
    } else {
      frames.pop_back();
    }
  }
}

void ConstructorCalls::enterScan(std::vector<ScanFrame> &frames, const FunctionDecl *fn,
                                 const CallExpr *origin,
                                 llvm::SmallPtrSetImpl<const FunctionDecl *> &seen) {
  if (!fn->body || !fn->thisVar()) {
    return;
  }
  auto [_, isFirstVisit] = seen.insert(fn);
  if (isFirstVisit) {
    std::vector<const Expr *> exprs = AstWalker::exprsIn(fn->body, false);
    frames.push_back(ScanFrame{fn, origin, std::move(exprs), 0});
  }
}

std::optional<ScanTarget>
ConstructorCalls::takeNextTarget(ScanFrame &frame, const ClassDecl *cls, const FunctionDecl *ctor,
                                 llvm::SmallPtrSetImpl<const FunctionDecl *> &seen) {
  while (frame.next < frame.exprs.size()) {
    const Expr *e = frame.exprs[frame.next];
    frame.next += 1;
    const CallExpr *call = this->selfCall(frame, e);
    if (call == nullptr) {
      continue;
    }
    const CallExpr *site = frame.origin ? frame.origin : call;
    ResultNotOverridden target = ConstructorCalls::overriddenCall(cls, ctor, call, site);
    if (target.isErr()) {
      this->state.getDiagnostics().error(site->loc(), target.error().message);
      this->state.getClassStore().markCallReported(site);
      continue;
    }
    if (!frame.origin) {
      seen.clear();
    }
    seen.insert(frame.fn);
    return ScanTarget{target.value(), site};
  }

  return std::nullopt;
}

const CallExpr *ConstructorCalls::selfCall(const ScanFrame &frame, const Expr *e) const {
  auto *call = dyn_cast<CallExpr>(e);
  bool isOriginReported = frame.origin && this->state.getClassStore().isCallReported(frame.origin);
  if (!call || !call->method() || isOriginReported) {
    return nullptr;
  }
  const NameExpr *name = ConstructorCalls::receiverName(call);
  bool isOnSelf = name != nullptr && name->var() == frame.fn->thisVar();

  return isOnSelf ? call : nullptr;
}

}
