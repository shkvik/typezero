#include "compiler/semantic/semantic_state/v2/inference/call_cycle_context/call_cycle_context.h"

namespace tz {

void CallCycleContext::addCycle(std::vector<FuncSymbol *> cycle) {
  int id = int(this->cycles.size());
  for (FuncSymbol *function : cycle) {
    this->cycleOf[function] = id;
  }
  this->cycles.push_back(std::move(cycle));
}

bool CallCycleContext::isInCycle(const FuncSymbol *function) const {
  return this->cycleOf.count(function) > 0;
}

std::vector<FuncSymbol *> CallCycleContext::getCycleOf(const FuncSymbol *function) const {
  auto found = this->cycleOf.find(function);
  if (found == this->cycleOf.end()) {
    return {};
  }

  return this->cycles[found->second];
}

void CallCycleContext::addDeclaredFunction(FunctionDecl *functionDecl) {
  this->declaredFunctions.push_back(functionDecl);
}

std::vector<FunctionDecl *> CallCycleContext::getDeclaredFunctions() const {
  return this->declaredFunctions;
}

}
