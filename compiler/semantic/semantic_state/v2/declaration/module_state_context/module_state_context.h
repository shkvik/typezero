#pragma once

#include "compiler/program/module/module.h"
#include "compiler/semantic/semantic_state/module_states/module_states_types.h"
#include "llvm/ADT/DenseMap.h"
#include <vector>

namespace tz {

class ModuleStateContext {
public:
  void leaveLoading();
  void markTopLevelDone();
  void enterBuiltinModule();
  void leaveBuiltinModule();
  void prepareModules(int count);
  void enterLoading(Module *module);
  void rememberCurrentModule(Module *module);
  void rememberDeclModule(const Stmt *decl, Module *module);


  bool isTopLevelDone() const;
  bool isInsideBuiltinModule() const;
  bool hasDeclModule(const Stmt *decl) const;
  bool hasModuleState(const Module *module) const;


  Module *getCurrentModule() const;
  Module *getLoadingModule() const;
  Module *findDeclModule(const Stmt *decl) const;


  ModuleState &getModuleState(const Module *module);
  const ModuleState &getModuleState(const Module *module) const;

private:
  bool topLevelDone = false;
  int builtinModuleDepth = 0;
  Module *currentModule = nullptr;
  std::vector<Module *> loadingModules;
  std::vector<ModuleState> moduleStates;
  llvm::DenseMap<const Stmt *, Module *> declModules;
};

}
