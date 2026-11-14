#include "compiler/semantic/code/classes/class_bodies/late_class_walks/late_class_walks.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/semantic/declare/class_tree/class_tree.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

LateClassWalks::LateClassWalks(SemanticState &owner, ClassFacts &sharedFacts)
    : facts(sharedFacts),
      state(owner) {}

void LateClassWalks::pushLateClassTasks() {
  return this->state.walk.tasks.pushTask(NextLateClass{});
}

void LateClassWalks::nextLateClass() {
  auto &late = this->state.code.late;
  auto &tasks = this->state.walk.tasks;
  if (late.hasUncheckedLateClasses()) {
    auto *classDecl = late.takeUncheckedLateClass();
    tasks.pushTask(NextLateClass{});
    if (!classDecl->checkOnly) {
      return tasks.pushTask(WalkLateClass{.classDecl = classDecl});
    }
    return;
  }
  auto reported = std::vector<InstanceRecord *>();
  for (const LateError &error : late.getFirstLateErrors()) {
    if (!error.instance->failed) {
      this->markLateFailure(error);
      reported.push_back(error.instance);
    }
  }
  for (InstanceRecord *instance : llvm::reverse(reported)) {
    tasks.pushTask(ReportInstance{.instance = instance});
  }
}

void LateClassWalks::markLateFailure(const LateError &error) {
  InstanceRecord *instance = error.instance;
  instance->failed = true;
  instance->message = error.first.message;
  instance->errorLoc = error.first.loc;
}

void LateClassWalks::walkLateClass(const WalkLateClass &task) {
  ClassDecl *classDecl = task.classDecl;
  auto &tasks = this->state.walk.tasks;
  auto *home = this->state.declaration.modules.findDeclModule(classDecl);
  bool isHomeUsed = home != nullptr && this->state.walk.scopes.isTopLevel();
  auto *owner = this->state.code.late.findLateOwner(classDecl);
  auto *outerInstance = this->state.generics.copies.getCheckingInstance();
  if (owner != nullptr) {
    this->state.generics.copies.rememberCheckingInstance(owner);
    this->state.diagnostics.pushList(DiagnosticRoute::OWNER);
  }
  this->state.walk.flow.replaceFlow(Flow());
  tasks.pushTask(FinishLateClassWalk{.owner = owner, .outerInstance = outerInstance});
  tasks.pushTask(WalkClassBodies{.classDecl = classDecl});
  if (isHomeUsed) {
    return tasks.pushTask(UseModule{.module = home});
  }
}

void LateClassWalks::finishLateClassWalk(const FinishLateClassWalk &task) {
  this->state.generics.copies.rememberCheckingInstance(task.outerInstance);
  if (task.owner == nullptr) {
    return;
  }
  auto classDiagnostics = this->state.diagnostics.popList();

  return this->state.code.late.addLateErrors(task.owner, classDiagnostics);
}

void LateClassWalks::rememberEmittedLateClasses() {
  for (ClassDecl *classDecl : this->state.declaration.classes.getClasses()) {
    bool isMadeLate = classDecl->expression() && classDecl->type != nullptr;
    bool isLate = classDecl->generic() != nullptr || isMadeLate;
    if (isLate && classDecl->isEmitted()) {
      this->state.inventory.lateClasses.push_back(classDecl);
    }
  }
}

void LateClassWalks::noteMethodNames(const ClassDecl *classDecl) {
  for (const FunctionDecl *functionDecl : classDecl->methods) {
    if (!functionDecl->isStatic()) {
      this->state.code.late.addLateMethodName(functionDecl->name);
    }
  }
  for (const ClassField &field : classDecl->fields) {
    bool isArrowMethod =
        !field.isStatic() && field.init != nullptr && isa<FunctionExpr>(field.init);
    if (isArrowMethod) {
      this->state.code.late.addLateMethodName(field.name);
    }
  }
}

void LateClassWalks::markLateOverrides(SourceFile &program) {
  for (Stmt *item : program.items) {
    auto *classDecl = dyn_cast<ClassDecl>(item);
    if (classDecl != nullptr && classDecl->baseExpr != nullptr) {
      this->noteMethodNames(classDecl);
    }
  }
  auto nodes = PlacedNodes::collect(program.items, {});
  for (const PlacedExpr &placed : nodes.getExprs()) {
    auto *classExpr = dyn_cast<ClassExpr>(placed.expr);
    if (classExpr != nullptr && this->isExtending(classExpr->decl)) {
      this->noteMethodNames(classExpr->decl);
    }
  }
  for (ClassDecl *classDecl : this->state.declaration.classes.getClasses()) {
    this->markClassOverrides(classDecl);
  }
}

bool LateClassWalks::isExtending(const ClassDecl *classDecl) const {
  return classDecl->baseExpr != nullptr || classDecl->base.has_value();
}

void LateClassWalks::markClassOverrides(ClassDecl *classDecl) {
  auto &late = this->state.code.late;
  if (!late.hasLateMethodNames() || classDecl->literal() != nullptr) {
    return;
  }
  for (FunctionDecl *functionDecl : this->facts.ownMethods(classDecl)) {
    Overriding *overriding = functionDecl->overriding();
    if (late.isLateMethodName(functionDecl->name) && overriding != nullptr) {
      overriding->overridden = true;
      overriding->openToLate = true;
    }
  }
}

void LateClassWalks::walkConstructorCalls() {
  for (ClassDecl *classDecl : this->state.declaration.classes.getClasses()) {
    if (this->hasOverridingChildren(classDecl)) {
      this->walkClassConstructors(classDecl);
    }
  }
}

bool LateClassWalks::hasOverridingChildren(const ClassDecl *classDecl) const {
  bool isPlainClass = classDecl->literal() == nullptr && classDecl->generic() == nullptr;

  return isPlainClass && !classDecl->children.empty();
}

void LateClassWalks::walkClassConstructors(const ClassDecl *classDecl) {
  for (const FunctionDecl *constructor : classDecl->constructors) {
    this->scanConstructorCalls(constructor, classDecl);
  }
  if (classDecl->init != nullptr) {
    this->scanConstructorCalls(classDecl->init, classDecl);
  }
}

const NameExpr *LateClassWalks::receiverName(const CallExpr *call) const {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member == nullptr) {
    return nullptr;
  }
  const Expr *object = ExprParts::withoutCasts(member->object);

  return dyn_cast<NameExpr>(object);
}

const ClassDecl *LateClassWalks::overridingChild(const ClassDecl *classDecl,
                                                 const FunctionDecl *method) const {
  for (const ClassDecl *child : ClassTree::descendants(classDecl)) {
    for (FunctionDecl *other : this->facts.ownMethods(child)) {
      if (this->facts.isOverridingTransitively(other, method)) {
        return child;
      }
    }
  }

  return nullptr;
}

LateClassWalks::ResultNotOverridden LateClassWalks::overriddenCall(const ClassDecl *classDecl,
                                                                   const FunctionDecl *constructor,
                                                                   const CallExpr *call,
                                                                   const CallExpr *site) const {
  FunctionDecl *target = call->method()->decl;
  bool isOverridable = call->virtualCall() || !classDecl->derivesFrom(target->cls());
  const ClassDecl *child = call->virtualCall() ? this->overridingChild(classDecl, target) : nullptr;
  const ClassDecl *overrider = child != nullptr ? child : target->cls();
  bool isDirect = site == call;
  if (isOverridable && constructor->isConstructor() && isDirect) {
    auto message =
        ErrorTextOverriddenCall::ctorCall(classDecl->name, target->name, overrider->name);

    return err(CodeClassesRegistryOverriddenCallError{
        .code = CodeClassesRegistryOverriddenCallErrorCode::CONSTRUCTOR_CALLS_OVERRIDE,
        .message = message,
    });
  }
  if (isOverridable && constructor->isConstructor()) {
    auto message =
        ErrorTextOverriddenCall::ctorReach(classDecl->name, target->name, overrider->name);

    return err(CodeClassesRegistryOverriddenCallError{
        .code = CodeClassesRegistryOverriddenCallErrorCode::CONSTRUCTOR_REACHES_OVERRIDE,
        .message = message,
    });
  }
  if (isOverridable && isDirect) {
    auto message =
        ErrorTextOverriddenCall::fieldsCall(classDecl->name, target->name, overrider->name);

    return err(CodeClassesRegistryOverriddenCallError{
        .code = CodeClassesRegistryOverriddenCallErrorCode::INITIALIZERS_CALL_OVERRIDE,
        .message = message,
    });
  }
  if (isOverridable) {
    auto message =
        ErrorTextOverriddenCall::fieldsReach(classDecl->name, target->name, overrider->name);

    return err(CodeClassesRegistryOverriddenCallError{
        .code = CodeClassesRegistryOverriddenCallErrorCode::INITIALIZERS_REACH_OVERRIDE,
        .message = message,
    });
  }

  return ok(target);
}

void LateClassWalks::scanConstructorCalls(const FunctionDecl *constructor,
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

void LateClassWalks::enterScan(std::vector<ScanFrame> &frames, const FunctionDecl *functionDecl,
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
      .exprs = AstWalker::ownExprsIn(functionDecl->body),
      .next = 0,
  };
  frames.push_back(std::move(frame));
}

std::optional<ScanTarget>
LateClassWalks::takeNextTarget(ScanFrame &frame, const ClassDecl *classDecl,
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

std::optional<ScanTarget> LateClassWalks::followedTarget(const ScanFrame &frame, const Expr *expr,
                                                         const ClassDecl *classDecl,
                                                         const FunctionDecl *constructor) {
  const auto *call = this->selfCall(frame, expr);
  if (call == nullptr) {
    return std::nullopt;
  }
  const CallExpr *site = frame.origin != nullptr ? frame.origin : call;
  auto target = this->overriddenCall(classDecl, constructor, call, site);
  if (target.isErr()) {
    this->state.diagnostics.error(site->loc(), target.error().message);
    this->state.code.constructors.markCallReported(site);

    return std::nullopt;
  }

  return ScanTarget{.functionDecl = target.value(), .site = site};
}

void LateClassWalks::markScanned(const ScanFrame &frame,
                                 llvm::SmallPtrSetImpl<const FunctionDecl *> &seen) const {
  if (frame.origin == nullptr) {
    seen.clear();
  }
  seen.insert(frame.functionDecl);
}

const CallExpr *LateClassWalks::selfCall(const ScanFrame &frame, const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);
  bool isOriginReported =
      frame.origin != nullptr && this->state.code.constructors.isCallReported(frame.origin);
  if (call == nullptr || call->method() == nullptr || isOriginReported) {
    return nullptr;
  }
  const auto *name = this->receiverName(call);
  bool isOnSelf = name != nullptr && name->var() == frame.functionDecl->thisVar();

  return isOnSelf ? call : nullptr;
}

}
