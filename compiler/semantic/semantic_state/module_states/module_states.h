#pragma once

#include "compiler/program/module/module.h"
#include "compiler/semantic/semantic_state/module_states/module_states_types.h"
#include "llvm/ADT/DenseMap.h"
#include <vector>

namespace tz {

class ModuleStates {
public:
  void prepareModules(int count);
  bool hasModuleState(const Module *module) const;
  ModuleState &getModuleState(const Module *module);
  void rememberCurrentModule(Module *module);
  Module *getCurrentModule() const;
  void enterLoading(Module *module);
  void leaveLoading();
  Module *getLoadingModule() const;
  void rememberDeclModule(const Stmt *decl, Module *module);
  bool hasDeclModule(const Stmt *decl) const;
  Module *findDeclModule(const Stmt *decl) const;
  void markTopLevelDone();
  bool isTopLevelDone() const;

private:
  std::vector<ModuleState> moduleStates;
  Module *currentModule = nullptr;
  std::vector<Module *> loadingModules;
  llvm::DenseMap<const Stmt *, Module *> declModules;
  bool topLevelDone = false;
};

}
