#include "compiler/pipeline/check_program/check_state/function_stack/function_stack.h"

namespace tz {

void FunctionStack::enterFunction(FunctionDecl *fn) {
  this->functions.push_back(fn);
}

void FunctionStack::leaveFunction() {
  this->functions.pop_back();
}

FunctionDecl *FunctionStack::getCurrentDecl() const {
  return this->functions.empty() ? nullptr : this->functions.back();
}

FunctionDecl *FunctionStack::getOutermostDecl() const {
  return this->functions.empty() ? nullptr : this->functions.front();
}

bool FunctionStack::isInsideFunction() const {
  return !this->functions.empty();
}

bool FunctionStack::isCheckingFunction(const FunctionDecl *fn) const {
  return llvm::is_contained(this->functions, fn);
}

size_t FunctionStack::getFunctionDepth() const {
  return this->functions.size();
}

std::vector<FunctionDecl *> FunctionStack::getFunctions() const {
  return this->functions;
}

void FunctionStack::rememberCurrentFunction(FuncSymbol *fn) {
  this->currentFunction = fn;
}

FuncSymbol *FunctionStack::getCurrentFunction() const {
  return this->currentFunction;
}

void FunctionStack::enterGroup(NestedGroup *group) {
  this->groups.push_back(group);
}

void FunctionStack::leaveGroup() {
  this->groups.pop_back();
}

bool FunctionStack::isAssignedInGroup(llvm::StringRef name) const {
  for (const NestedGroup *group : this->groups) {
    if (llvm::is_contained(group->assignedNames, name)) {
      return true;
    }
  }

  return false;
}

void FunctionStack::rememberReturnCollector(std::vector<ReturnStmt *> *collector) {
  this->returns = collector;
}

std::vector<ReturnStmt *> *FunctionStack::getReturnCollector() const {
  return this->returns;
}

void FunctionStack::addCollectedReturn(ReturnStmt *ret) {
  this->returns->push_back(ret);
}

std::vector<ReturnStmt *> FunctionStack::getCollectedReturns() const {
  return *this->returns;
}

void FunctionStack::rememberYieldCollector(std::vector<YieldExpr *> *collector) {
  this->yields = collector;
}

std::vector<YieldExpr *> *FunctionStack::getYieldCollector() const {
  return this->yields;
}

bool FunctionStack::isCollectingYields() const {
  return this->yields != nullptr;
}

void FunctionStack::addCollectedYield(YieldExpr *yield) {
  this->yields->push_back(yield);
}

void FunctionStack::enterTrial() {
  this->trial = true;
}

void FunctionStack::leaveTrial() {
  this->trial = false;
}

bool FunctionStack::isInsideTrial() const {
  return this->trial;
}

void FunctionStack::rememberEarlyFunction(const FuncSymbol *fn) {
  this->early = fn;
}

void FunctionStack::forgetEarlyFunction() {
  this->early = nullptr;
}

const FuncSymbol *FunctionStack::getEarlyFunction() const {
  return this->early;
}

void FunctionStack::enterCallback(const Signature *expected) {
  this->callbacks.push_back(expected);
}

void FunctionStack::leaveCallback() {
  this->callbacks.pop_back();
}

const Signature *FunctionStack::getExpectedCallback() const {
  return this->callbacks.empty() ? nullptr : this->callbacks.back();
}

void FunctionStack::enterAwaitLoop() {
  this->awaitLoopDepth += 1;
}

void FunctionStack::leaveAwaitLoop() {
  this->awaitLoopDepth -= 1;
}

bool FunctionStack::isInsideAwaitLoop() const {
  return this->awaitLoopDepth > 0;
}

void FunctionStack::enterOptionalChain() {
  this->shortCircuits.push_back(false);
}

void FunctionStack::leaveOptionalChain() {
  this->shortCircuits.pop_back();
}

void FunctionStack::markShortCircuit() {
  this->shortCircuits.back() = true;
}

bool FunctionStack::isShortCircuiting() const {
  return this->shortCircuits.back();
}

FunctionSnapshot FunctionStack::saveBody() {
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

void FunctionStack::keepGroupFunctions(const NestedGroup *group) {
  this->functions.resize(group->functionDepth);
  while (!this->groups.empty() && this->groups.back() != group) {
    this->groups.pop_back();
  }
}

void FunctionStack::forgetOuterFunctions() {
  this->functions.clear();
  this->groups.clear();
  this->trial = false;
}

void FunctionStack::restoreBody(FunctionSnapshot saved) {
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
