#include "compiler/semantic/inference_v2/returns/return_bodies/return_bodies.h"

namespace tz {

ReturnBodies::ReturnBodies(SemanticState &owner, ReturnRules &sharedReturnRules)
    : state(owner),
      returnRules(sharedReturnRules) {}

void ReturnBodies::openBodyCheck(const OpenBodyCheck &task) {
  FunctionDecl *functionDecl = task.function->decl;
  bool isClosure = functionDecl->isClosure();
  auto route = this->bodyRoute(isClosure, task.lateOwner);
  this->state.diagnostics.pushList(route);
  bool isTopLevelEarly = !isClosure && !this->state.declaration.modules.isTopLevelDone();
  bool isGroupEarly = functionDecl->group != nullptr && !functionDecl->group->finished;
  if (isTopLevelEarly || isGroupEarly) {
    this->state.walk.functions.rememberEarlyFunction(task.function);
  }
}

DiagnosticRoute ReturnBodies::bodyRoute(bool isClosure, const InstanceRecord *lateOwner) const {
  if (isClosure) {
    return DiagnosticRoute::PARENT;
  }
  if (lateOwner != nullptr) {
    return DiagnosticRoute::OWNER;
  }

  return DiagnosticRoute::PROGRAM;
}

void ReturnBodies::closeBodyCheck(const CloseBodyCheck &task) {
  auto bodyDiagnostics = this->state.diagnostics.popList();
  if (task.lateOwner != nullptr) {
    this->state.code.late.addLateErrors(task.lateOwner, bodyDiagnostics);
  }
}

void ReturnBodies::enterReturnBody(const EnterReturnBody &task) {
  return this->enterBody(task.functionDecl);
}

void ReturnBodies::enterLoadBody() {
  return this->enterBody(nullptr);
}

void ReturnBodies::enterBody(const FunctionDecl *functionDecl) {
  auto &tasks = this->state.walk.tasks;
  auto &scopes = this->state.walk.scopes;
  auto saved = this->state.saveBody();
  this->state.inference.returns.pushSavedBody(std::move(saved));
  NestedGroup *group = functionDecl != nullptr ? functionDecl->group : nullptr;
  if (group != nullptr) {
    scopes.keepGroupScopes(group);
    this->state.walk.functions.keepGroupFunctions(group);

    return scopes.alignBranchesWithScopes();
  }
  bool isTopLevel = functionDecl == nullptr || !functionDecl->isExpression();
  if (!isTopLevel) {
    return scopes.alignBranchesWithScopes();
  }
  scopes.keepTopScopes();
  this->state.walk.functions.forgetOuterFunctions();
  this->state.walk.block.forgetOtherCaseVars();
  tasks.pushTask(FinishTopLevelEntry{.functionDecl = functionDecl});
  if (functionDecl == nullptr) {
    return;
  }

  return tasks.pushTask(EnterHomeModule{.functionDecl = functionDecl});
}

void ReturnBodies::finishTopLevelEntry(const FinishTopLevelEntry &task) {
  auto &copies = this->state.generics.copies;
  copies.forgetCheckingInstance();
  auto *owner = this->returnRules.lateOwnerOf(task.functionDecl);
  if (owner != nullptr) {
    copies.rememberCheckingInstance(owner);
  }

  return this->state.walk.scopes.alignBranchesWithScopes();
}

void ReturnBodies::leaveReturnBody() {
  auto saved = this->state.inference.returns.takeSavedBody();

  return this->state.restoreBody(std::move(saved));
}

void ReturnBodies::leaveLoadBody() {
  return this->leaveReturnBody();
}

void ReturnBodies::walkFreshCopy(const WalkFreshCopy &task) {
  auto *copy = this->returnRules.freshCopy(task.functionDecl);

  return this->state.walk.tasks.pushTask(WalkFunctionBody{.functionDecl = copy});
}

}
