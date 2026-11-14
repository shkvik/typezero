#include "compiler/semantic/semantic_state/v2/walk/function_stack_context/function_stack_context.h"

namespace tz {

void FunctionStackContext::enterFunction(FunctionDecl *functionDecl) {
  this->functions.push_back(functionDecl);
}

void FunctionStackContext::leaveFunction() {
  this->functions.pop_back();
}

FunctionDecl *FunctionStackContext::getCurrentDecl() const {
  return this->functions.empty() ? nullptr : this->functions.back();
}

FunctionDecl *FunctionStackContext::getOutermostDecl() const {
  return this->functions.empty() ? nullptr : this->functions.front();
}

bool FunctionStackContext::isInsideFunction() const {
  return !this->functions.empty();
}

bool FunctionStackContext::isCheckingFunction(const FunctionDecl *functionDecl) const {
  return llvm::is_contained(this->functions, functionDecl);
}

size_t FunctionStackContext::getFunctionDepth() const {
  return this->functions.size();
}

std::vector<FunctionDecl *> FunctionStackContext::getFunctions() const {
  return this->functions;
}

void FunctionStackContext::rememberCurrentFunction(FuncSymbol *function) {
  this->currentFunction = function;
}

FuncSymbol *FunctionStackContext::getCurrentFunction() const {
  return this->currentFunction;
}

void FunctionStackContext::enterGroup(NestedGroup *group) {
  this->groups.push_back(group);
}

void FunctionStackContext::leaveGroup() {
  this->groups.pop_back();
}

bool FunctionStackContext::isAssignedInGroup(llvm::StringRef name) const {
  for (const NestedGroup *group : this->groups) {
    if (llvm::is_contained(group->assignedNames, name)) {
      return true;
    }
  }

  return false;
}

void FunctionStackContext::rememberReturnCollector(std::vector<ReturnStmt *> *collector) {
  this->returns = collector;
}

std::vector<ReturnStmt *> *FunctionStackContext::getReturnCollector() const {
  return this->returns;
}

void FunctionStackContext::addCollectedReturn(ReturnStmt *returnStmt) {
  this->returns->push_back(returnStmt);
}

std::vector<ReturnStmt *> FunctionStackContext::getCollectedReturns() const {
  return *this->returns;
}

void FunctionStackContext::rememberYieldCollector(std::vector<YieldExpr *> *collector) {
  this->yields = collector;
}

std::vector<YieldExpr *> *FunctionStackContext::getYieldCollector() const {
  return this->yields;
}

bool FunctionStackContext::isCollectingYields() const {
  return this->yields != nullptr;
}

void FunctionStackContext::addCollectedYield(YieldExpr *yield) {
  this->yields->push_back(yield);
}

void FunctionStackContext::enterTrial() {
  this->trial = true;
}

void FunctionStackContext::leaveTrial() {
  this->trial = false;
}

bool FunctionStackContext::isInsideTrial() const {
  return this->trial;
}

void FunctionStackContext::rememberEarlyFunction(const FuncSymbol *function) {
  this->early = function;
}

void FunctionStackContext::forgetEarlyFunction() {
  this->early = nullptr;
}

const FuncSymbol *FunctionStackContext::getEarlyFunction() const {
  return this->early;
}

void FunctionStackContext::enterCallback(const std::optional<Signature> &expected) {
  this->callbacks.push_back(expected);
}

void FunctionStackContext::leaveCallback() {
  this->callbacks.pop_back();
}

const Signature *FunctionStackContext::getExpectedCallback() const {
  if (this->callbacks.empty() || !this->callbacks.back().has_value()) {
    return nullptr;
  }

  return &*this->callbacks.back();
}

void FunctionStackContext::enterAwaitLoop() {
  this->awaitLoopDepth += 1;
}

void FunctionStackContext::leaveAwaitLoop() {
  this->awaitLoopDepth -= 1;
}

bool FunctionStackContext::isInsideAwaitLoop() const {
  return this->awaitLoopDepth > 0;
}

void FunctionStackContext::enterOptionalChain() {
  this->shortCircuits.push_back(false);
}

void FunctionStackContext::leaveOptionalChain() {
  this->shortCircuits.pop_back();
}

void FunctionStackContext::markShortCircuit() {
  this->shortCircuits.back() = true;
}

bool FunctionStackContext::isShortCircuiting() const {
  return this->shortCircuits.back();
}

FunctionSnapshot FunctionStackContext::saveBody() {
  FunctionSnapshot saved;
  saved.functions = this->functions;
  saved.groups = this->groups;
  saved.function = this->currentFunction;
  saved.chainShortCircuits = this->shortCircuits.back();
  saved.returns = this->returns;
  saved.yields = this->yields;
  saved.trial = this->trial;
  saved.early = this->early;
  this->currentFunction = nullptr;
  this->shortCircuits.back() = false;
  this->returns = nullptr;
  this->yields = nullptr;

  return saved;
}

void FunctionStackContext::keepGroupFunctions(const NestedGroup *group) {
  this->functions.resize(group->functionDepth);
  while (!this->groups.empty() && this->groups.back() != group) {
    this->groups.pop_back();
  }
}

void FunctionStackContext::forgetOuterFunctions() {
  this->functions.clear();
  this->groups.clear();
  this->trial = false;
}

void FunctionStackContext::restoreBody(FunctionSnapshot saved) {
  this->functions = std::move(saved.functions);
  this->groups = std::move(saved.groups);
  this->currentFunction = saved.function;
  this->shortCircuits.back() = saved.chainShortCircuits;
  this->returns = saved.returns;
  this->yields = saved.yields;
  this->trial = saved.trial;
  this->early = saved.early;
}

}
