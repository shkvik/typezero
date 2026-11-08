#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/declare/module_declarer/module_declarer_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ModuleDeclarer {
public:
  using ResultExportTarget = Result<ModuleBinding, ModuleDeclarerFindExportError>;

  explicit ModuleDeclarer(SemanticState &owner);

  void useModule(Module *module);
  std::vector<FunctionDecl *> declareModules(SourceFile &program);
  const llvm::StringMap<ModuleExport> &exportsOf(Module *module);
  std::optional<ModuleBinding> resolveLocal(Module *module, llvm::StringRef name);
  ResultExportTarget findExport(Module *from, llvm::StringRef path, const ModuleName &name);

private:
  using ResultStarIsUnique = Result<const ExportAll *, ModuleDeclarerStarExportError>;
  using ResultExportUnique = Result<const ModuleName *, ModuleDeclarerFreeExportError>;
  using ResultLocalIsKnown = Result<const ModuleName *, ModuleDeclarerLocalExportError>;
  using ResultSourceUsable = Result<const ExportSource *, ModuleDeclarerExportSourceError>;

  static ResultSourceUsable exportSource(const ExportSource *source);
  static ResultExportUnique freeExport(const ModuleState &here, const ModuleName &name);
  static ResultLocalIsKnown localExport(const Module *module, const ModuleName &name, bool found);
  static ResultStarIsUnique starExport(llvm::StringRef name, const ModuleExport *ours,
                                       const ExportAll *all, const ModuleBinding &target);

  void exportName(ModuleState &here, Module *module, const ExportSource *source,
                  const ModuleName &name);

  SemanticState &state;
};

}
