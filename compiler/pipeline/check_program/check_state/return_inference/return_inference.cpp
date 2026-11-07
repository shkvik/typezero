#include "compiler/pipeline/check_program/check_state/return_inference/return_inference.h"

namespace tz {

void ReturnInference::enterInference(const FuncSymbol *fn) {
  this->inferring.push_back(fn);
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

bool ReturnInference::isSuspended(const FuncSymbol *fn) const {
  return this->suspended.contains(fn);
}

void ReturnInference::forgetSuspended(const FuncSymbol *fn) {
  this->suspended.erase(fn);
}

void ReturnInference::addCycle(std::vector<FuncSymbol *> cycle) {
  int id = int(this->cycles.size());
  for (FuncSymbol *fn : cycle) {
    this->cycleOf[fn] = id;
  }
  this->cycles.push_back(std::move(cycle));
}

bool ReturnInference::isInCycle(const FuncSymbol *fn) const {
  return this->cycleOf.count(fn) > 0;
}

std::vector<FuncSymbol *> ReturnInference::getCycleOf(const FuncSymbol *fn) const {
  auto found = this->cycleOf.find(fn);
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

void ReturnInference::addLiftedFunction(FunctionDecl *fn) {
  this->liftedFunctions.push_back(fn);
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
