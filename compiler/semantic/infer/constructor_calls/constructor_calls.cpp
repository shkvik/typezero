#include "compiler/semantic/infer/constructor_calls/constructor_calls.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/declare/class_tree/class_tree.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

ConstructorCalls::ConstructorCalls(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->classLinks = &all.classLinks;
}

void ConstructorCalls::walkConstructorCalls() {
  for (ClassDecl *classDecl : this->state.getClassStore().getClasses()) {
    if (this->hasOverridingChildren(classDecl)) {
      this->walkClassConstructors(classDecl);
    }
  }
}

bool ConstructorCalls::hasOverridingChildren(const ClassDecl *classDecl) const {
  bool isPlainClass = classDecl->literal() == nullptr && classDecl->generic() == nullptr;

  return isPlainClass && !classDecl->children.empty();
}

void ConstructorCalls::walkClassConstructors(const ClassDecl *classDecl) {
  for (const FunctionDecl *constructor : classDecl->constructors) {
    this->scanConstructorCalls(constructor, classDecl);
  }
  if (classDecl->init != nullptr) {
    this->scanConstructorCalls(classDecl->init, classDecl);
  }
}

const NameExpr *ConstructorCalls::receiverName(const CallExpr *call) const {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member == nullptr) {
    return nullptr;
  }
  const Expr *object = ExprParts::withoutCasts(member->object);

  return dyn_cast<NameExpr>(object);
}

const ClassDecl *ConstructorCalls::overridingChild(const ClassDecl *classDecl,
                                                   const FunctionDecl *method) const {
  for (const ClassDecl *child : ClassTree::descendants(classDecl)) {
    for (FunctionDecl *other : this->classLinks->ownMethods(child)) {
      if (this->classLinks->overridesTransitively(other, method)) {
        return child;
      }
    }
  }

  return nullptr;
}

ConstructorCalls::ResultNotOverridden
ConstructorCalls::overriddenCall(const ClassDecl *classDecl, const FunctionDecl *constructor,
                                 const CallExpr *call, const CallExpr *site) const {
  FunctionDecl *target = call->method()->decl;
  bool isOverridable = call->virtualCall() || !classDecl->derivesFrom(target->cls());
  const ClassDecl *child = call->virtualCall() ? this->overridingChild(classDecl, target) : nullptr;
  const ClassDecl *overrider = child != nullptr ? child : target->cls();
  bool isDirect = site == call;
  if (isOverridable && constructor->isConstructor() && isDirect) {
    auto message =
        ErrorTextOverriddenCall::ctorCall(classDecl->name, target->name, overrider->name);

    return err(ConstructorCallsOverriddenCallError{
        .code = ConstructorCallsOverriddenCallErrorCode::CONSTRUCTOR_CALLS_OVERRIDE,
        .message = message,
    });
  }
  if (isOverridable && constructor->isConstructor()) {
    auto message =
        ErrorTextOverriddenCall::ctorReach(classDecl->name, target->name, overrider->name);

    return err(ConstructorCallsOverriddenCallError{
        .code = ConstructorCallsOverriddenCallErrorCode::CONSTRUCTOR_REACHES_OVERRIDE,
        .message = message,
    });
  }
  if (isOverridable && isDirect) {
    auto message =
        ErrorTextOverriddenCall::fieldsCall(classDecl->name, target->name, overrider->name);

    return err(ConstructorCallsOverriddenCallError{
        .code = ConstructorCallsOverriddenCallErrorCode::INITIALIZERS_CALL_OVERRIDE,
        .message = message,
    });
  }
  if (isOverridable) {
    auto message =
        ErrorTextOverriddenCall::fieldsReach(classDecl->name, target->name, overrider->name);

    return err(ConstructorCallsOverriddenCallError{
        .code = ConstructorCallsOverriddenCallErrorCode::INITIALIZERS_REACH_OVERRIDE,
        .message = message,
    });
  }

  return ok(target);
}

void ConstructorCalls::scanConstructorCalls(const FunctionDecl *constructor,
                                            const ClassDecl *classDecl) {
  auto seen = llvm::SmallPtrSet<const FunctionDecl *, 8>();
  auto frames = std::vector<ScanFrame>();
  this->enterScan(frames, constructor, nullptr, seen);
  while (!frames.empty()) {
    auto next = this->takeNextTarget(frames.back(), classDecl, constructor, seen);
    if (next.has_value()) {
      this->enterScan(frames, next->functionDecl, next->site, seen);
    } else {
      frames.pop_back();
    }
  }
}

void ConstructorCalls::enterScan(std::vector<ScanFrame> &frames, const FunctionDecl *functionDecl,
                                 const CallExpr *origin,
                                 llvm::SmallPtrSetImpl<const FunctionDecl *> &seen) const {
  if (functionDecl->body == nullptr || functionDecl->thisVar() == nullptr) {
    return;
  }
  bool isFirstVisit = seen.insert(functionDecl).second;
  if (!isFirstVisit) {
    return;
  }
  auto frame = ScanFrame{
      .functionDecl = functionDecl,
      .origin = origin,
      .exprs = AstWalker::exprsIn(functionDecl->body, false),
      .next = 0,
  };
  frames.push_back(std::move(frame));
}

std::optional<ScanTarget>
ConstructorCalls::takeNextTarget(ScanFrame &frame, const ClassDecl *classDecl,
                                 const FunctionDecl *constructor,
                                 llvm::SmallPtrSetImpl<const FunctionDecl *> &seen) {
  while (frame.next < frame.exprs.size()) {
    const Expr *expr = frame.exprs[frame.next];
    frame.next += 1;
    auto target = this->followedTarget(frame, expr, classDecl, constructor);
    if (target.has_value()) {
      this->markScanned(frame, seen);
      return target;
    }
  }

  return std::nullopt;
}

std::optional<ScanTarget> ConstructorCalls::followedTarget(const ScanFrame &frame, const Expr *expr,
                                                           const ClassDecl *classDecl,
                                                           const FunctionDecl *constructor) {
  const auto *call = this->selfCall(frame, expr);
  if (call == nullptr) {
    return std::nullopt;
  }
  const CallExpr *site = frame.origin != nullptr ? frame.origin : call;
  auto target = this->overriddenCall(classDecl, constructor, call, site);
  if (target.isErr()) {
    this->state.getDiagnostics().error(site->loc(), target.error().message);
    this->state.getClassStore().markCallReported(site);

    return std::nullopt;
  }

  return ScanTarget{.functionDecl = target.value(), .site = site};
}

void ConstructorCalls::markScanned(const ScanFrame &frame,
                                   llvm::SmallPtrSetImpl<const FunctionDecl *> &seen) const {
  if (frame.origin == nullptr) {
    seen.clear();
  }
  seen.insert(frame.functionDecl);
}

const CallExpr *ConstructorCalls::selfCall(const ScanFrame &frame, const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);
  bool isOriginReported =
      frame.origin != nullptr && this->state.getClassStore().isCallReported(frame.origin);
  if (call == nullptr || call->method() == nullptr || isOriginReported) {
    return nullptr;
  }
  const auto *name = this->receiverName(call);
  bool isOnSelf = name != nullptr && name->var() == frame.functionDecl->thisVar();

  return isOnSelf ? call : nullptr;
}

}
