#include "compiler/semantic/semantic_state/return_inference/return_inference.h"

namespace tz {

void ReturnInference::enterInference(const FuncSymbol *function) {
  this->inferring.push_back(function);
}

void ReturnInference::leaveInference() {
  this->inferring.pop_back();
}

void ReturnInference::enterModuleLoad() {
  this->suspendedBeforeLoads.push_back(this->suspended);
  this->suspended.insert(this->inferring.begin(), this->inferring.end());
}

void ReturnInference::leaveModuleLoad() {
  this->suspended = std::move(this->suspendedBeforeLoads.back());
  this->suspendedBeforeLoads.pop_back();
}

bool ReturnInference::isSuspended(const FuncSymbol *function) const {
  return this->suspended.contains(function);
}

void ReturnInference::forgetSuspended(const FuncSymbol *function) {
  this->suspended.erase(function);
}

void ReturnInference::addCycle(std::vector<FuncSymbol *> cycle) {
  int id = int(this->cycles.size());
  for (FuncSymbol *function : cycle) {
    this->cycleOf[function] = id;
  }
  this->cycles.push_back(std::move(cycle));
}

bool ReturnInference::isInCycle(const FuncSymbol *function) const {
  return this->cycleOf.count(function) > 0;
}

std::vector<FuncSymbol *> ReturnInference::getCycleOf(const FuncSymbol *function) const {
  auto found = this->cycleOf.find(function);
  if (found == this->cycleOf.end()) {
    return {};
  }

  return this->cycles[found->second];
}

void ReturnInference::rememberDeclaredFunctions(std::vector<FunctionDecl *> functions) {
  this->declaredFunctions = std::move(functions);
}

std::vector<FunctionDecl *> ReturnInference::getDeclaredFunctions() const {
  return this->declaredFunctions;
}

void ReturnInference::addLiftedFunction(FunctionDecl *functionDecl) {
  this->liftedFunctions.push_back(functionDecl);
}

std::vector<FunctionDecl *> ReturnInference::getLiftedFunctions() const {
  return this->liftedFunctions;
}

unsigned ReturnInference::takeSequenceNumber() {
  this->sequenceNumber += 1;

  return this->sequenceNumber;
}

unsigned ReturnInference::getSequenceNumber() const {
  return this->sequenceNumber;
}

}
