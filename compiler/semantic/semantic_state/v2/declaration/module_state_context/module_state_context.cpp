#include "compiler/semantic/semantic_state/v2/declaration/module_state_context/module_state_context.h"

namespace tz {

void ModuleStateContext::prepareModules(int count) {
  this->moduleStates.resize(count);
}

bool ModuleStateContext::hasModuleState(const Module *module) const {
  return module->order < this->moduleStates.size();
}

ModuleState &ModuleStateContext::getModuleState(const Module *module) {
  return this->moduleStates[module->order];
}

void ModuleStateContext::rememberCurrentModule(Module *module) {
  this->currentModule = module;
}

Module *ModuleStateContext::getCurrentModule() const {
  return this->currentModule;
}

void ModuleStateContext::enterLoading(Module *module) {
  this->loadingModules.push_back(module);
}

void ModuleStateContext::leaveLoading() {
  this->loadingModules.pop_back();
}

Module *ModuleStateContext::getLoadingModule() const {
  return this->loadingModules.empty() ? nullptr : this->loadingModules.back();
}

void ModuleStateContext::rememberDeclModule(const Stmt *decl, Module *module) {
  this->declModules[decl] = module;
}

bool ModuleStateContext::hasDeclModule(const Stmt *decl) const {
  return this->declModules.count(decl) > 0;
}

Module *ModuleStateContext::findDeclModule(const Stmt *decl) const {
  return this->declModules.lookup(decl);
}

void ModuleStateContext::markTopLevelDone() {
  this->topLevelDone = true;
}

bool ModuleStateContext::isTopLevelDone() const {
  return this->topLevelDone;
}

void ModuleStateContext::enterBuiltinModule() {
  this->builtinModuleDepth += 1;
}

void ModuleStateContext::leaveBuiltinModule() {
  this->builtinModuleDepth -= 1;
}

bool ModuleStateContext::isInsideBuiltinModule() const {
  return this->builtinModuleDepth > 0;
}

}
