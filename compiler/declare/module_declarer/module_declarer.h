#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/declare/module_declarer/module_declarer_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ModuleDeclarer {
public:
  using ResultExportFound = Result<Checker::Binding, ModuleDeclarerFindExportError>;

  explicit ModuleDeclarer(Checker &owner);

  void useModule(Module *module);
  std::vector<FunctionDecl *> declareModules(SourceFile &program);
  const llvm::StringMap<Checker::Export> &exportsOf(Module *module);
  std::optional<Checker::Binding> resolveLocal(Module *module, llvm::StringRef name);
  ResultExportFound findExport(Module *from, llvm::StringRef path, const ModuleName &name);

private:
  using ResultSourceIsLocal = Result<const ExportSource *, ModuleDeclarerExportSourceError>;
  using ResultLocalIsKnown = Result<const ModuleName *, ModuleDeclarerLocalExportError>;
  using ResultExportUnique = Result<const ModuleName *, ModuleDeclarerFreeExportError>;
  using ResultStarIsUnique = Result<const ExportAll *, ModuleDeclarerStarExportError>;

  static ResultSourceIsLocal exportSource(const ExportSource *source);
  static ResultExportUnique freeExport(const Checker::ModuleState &here, const ModuleName &name);
  static ResultLocalIsKnown localExport(const Module *module, const ModuleName &name, bool found);
  static ResultStarIsUnique starExport(llvm::StringRef name, const Checker::Export *ours,
                                       const ExportAll *all, const Checker::Binding &target);

  void exportName(Checker::ModuleState &here, Module *module, const ExportSource *source,
                  const ModuleName &name);

  Checker &checker;
};

}
