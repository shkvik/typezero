#include "compiler/semantic/semantic_state/v2/code/variable_context/variable_context.h"

namespace tz {

void VariableContext::enterVariableInit(llvm::StringRef name) {
  this->initializing.push_back(name);
}

void VariableContext::leaveVariableInit() {
  this->initializing.pop_back();
}

bool VariableContext::isInitializing(llvm::StringRef name) const {
  return llvm::is_contained(this->initializing, name);
}

void VariableContext::addUntyped(VarSymbol *variable, bool initUndefined) {
  this->untyped.push_back({variable, initUndefined, {}});
}

bool VariableContext::hasUntyped() const {
  return !this->untyped.empty();
}

void VariableContext::addUntypedAssignment(const VarSymbol *variable, const Type *type) {
  for (Untyped &entry : this->untyped) {
    if (entry.var == variable) {
      entry.assigned.push_back(type);
    }
  }
}

std::vector<Untyped> VariableContext::getUntyped() const {
  return this->untyped;
}

void VariableContext::forgetUntyped(const VarSymbol *variable) {
  llvm::erase_if(this->untyped, [&](const Untyped &entry) { return entry.var == variable; });
}

void VariableContext::addResultVar(VarSymbol *variable) {
  this->resultVars.push_back(variable);
}

std::vector<VarSymbol *> VariableContext::getResultVars() const {
  return this->resultVars;
}

void VariableContext::forgetResultVars() {
  this->resultVars.clear();
}

void VariableContext::addLiftedFunction(FunctionDecl *functionDecl) {
  this->liftedFunctions.push_back(functionDecl);
}

std::vector<FunctionDecl *> VariableContext::getLiftedFunctions() const {
  return this->liftedFunctions;
}

void VariableContext::enterModuleLoad() {
  this->initializingBeforeLoads.push_back(std::move(this->initializing));
  this->initializing.clear();
}

void VariableContext::leaveModuleLoad() {
  this->initializing = std::move(this->initializingBeforeLoads.back());
  this->initializingBeforeLoads.pop_back();
}

}
