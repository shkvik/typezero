#include "compiler/semantic/semantic_state/module_states/module_states.h"

namespace tz {

void ModuleStates::prepareModules(int count) {
  this->moduleStates.resize(count);
}

bool ModuleStates::hasModuleState(const Module *module) const {
  return module->order < this->moduleStates.size();
}

ModuleState &ModuleStates::getModuleState(const Module *module) {
  return this->moduleStates[module->order];
}

void ModuleStates::rememberCurrentModule(Module *module) {
  this->currentModule = module;
}

Module *ModuleStates::getCurrentModule() const {
  return this->currentModule;
}

void ModuleStates::enterLoading(Module *module) {
  this->loadingModules.push_back(module);
}

void ModuleStates::leaveLoading() {
  this->loadingModules.pop_back();
}

Module *ModuleStates::getLoadingModule() const {
  return this->loadingModules.empty() ? nullptr : this->loadingModules.back();
}

void ModuleStates::rememberDeclModule(const Stmt *decl, Module *module) {
  this->declModules[decl] = module;
}

bool ModuleStates::hasDeclModule(const Stmt *decl) const {
  return this->declModules.count(decl) > 0;
}

Module *ModuleStates::findDeclModule(const Stmt *decl) const {
  return this->declModules.lookup(decl);
}

void ModuleStates::markTopLevelDone() {
  this->topLevelDone = true;
}

bool ModuleStates::isTopLevelDone() const {
  return this->topLevelDone;
}

}
