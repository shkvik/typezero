#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/declaration/modules/declaration_modules_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "compiler/union/union/union.h"

namespace tz {

class StmtChecker;
class SemanticParts;
class EmitterChecker;
class ReturnInferrer;
class BuiltinModuleChecker;

enum class ExportStepKind { BAD_SOURCE, NAMED, STAR };

struct BadSourceStep {
  static constexpr ExportStepKind kind = ExportStepKind::BAD_SOURCE;
  llvm::SMLoc loc;
  std::string message;
};

struct NamedExportStep {
  static constexpr ExportStepKind kind = ExportStepKind::NAMED;
  const ExportSource *source;
  const ModuleName *name;
};

struct StarExportStep {
  static constexpr ExportStepKind kind = ExportStepKind::STAR;
  const ExportDecl *exportDecl;
  Module *from;
};

using ExportStep = Union<BadSourceStep, NamedExportStep, StarExportStep>;

struct ExportsFrame {
  Module *module;
  std::vector<ExportStep> steps;
  size_t nextStep;
};

struct ImportedName {
  Module *from;
  llvm::StringRef name;
};

class DeclarationModulesRegistry {
public:
  using ResultExportTarget = Result<ModuleBinding, DeclarationModulesRegistryFindExportError>;
  using ResultSourceIsLoaded = Result<const NameExpr *, DeclarationModulesRegistryCheckLoadedError>;

  DeclarationModulesRegistry(SemanticState &owner, SemanticParts &all);

  void bindImports();
  void useModule(Module *module);
  void declareNames(Module *module);
  void resolveImports(Module *module);
  void noteImportsInFunctions(SourceFile &program);
  void enterHomeModule(const FunctionDecl *functionDecl);
  void bindAllImports(const std::vector<Module *> &modules);
  void loadAllTopLevels(const std::vector<Module *> &modules);


  ScopeEntry lookupValue(llvm::StringRef name);
  ResultSourceIsLoaded checkLoaded(const NameExpr *name);
  const llvm::StringMap<ModuleExport> &exportsOf(Module *module);
  FunctionDecl *cycleCallee(FunctionDecl *functionDecl, llvm::StringRef name);

private:
  using ResultAliasIsUnused = Result<const ModuleName *, DeclarationModulesRegistryFreeNameError>;
  using ResultStarIsUnique = Result<const ExportAll *, DeclarationModulesRegistryStarExportError>;
  using ResultExportUnique = Result<const ModuleName *, DeclarationModulesRegistryFreeExportError>;
  using ResultLocalIsKnown = Result<const ModuleName *, DeclarationModulesRegistryLocalExportError>;
  using ResultImportSource =
      Result<const ImportDecl *, DeclarationModulesRegistryImportSourceError>;
  using ResultWholeImportOk =
      Result<const ImportDecl *, DeclarationModulesRegistryWholeImportError>;
  using ResultSourceUsable =
      Result<const ExportSource *, DeclarationModulesRegistryExportSourceError>;

  void enterModule(Module *module);
  void reportBadSource(const BadSourceStep &step);
  void finishExports(std::vector<ExportsFrame> &frames);
  void runExportStep(std::vector<ExportsFrame> &frames);
  void applyExportStep(Module *module, const ExportStep &step);
  void exportName(Module *module, const NamedExportStep &step);
  void loadTopLevel(Module *module, llvm::StringRef until = {});
  void addDeclaredItem(ModuleState &here, const Stmt *item) const;
  void mergeStarExport(Module *module, const StarExportStep &star);
  void resolveImport(Module *module, const ImportDecl &importDecl);
  void startExports(std::vector<ExportsFrame> &frames, Module *module);
  void bindImportedType(llvm::StringRef alias, const ModuleBinding &target);
  void addDeclared(ModuleState &here, llvm::StringRef name, llvm::SMLoc loc) const;
  void addNameSteps(const ExportDecl &exportDecl, std::vector<ExportStep> &steps) const;
  void importName(ModuleState &here, const ImportDecl &importDecl, const ModuleName &name);
  void bindImportedValue(ModuleState &here, llvm::StringRef alias, const ModuleBinding &target);
  void reportTaken(const ModuleState &here, const ModuleName &name,
                   const DeclarationModulesRegistryFreeNameError &error);


  bool loadEarly(Module *source, llvm::StringRef name);
  bool isImportedByName(const Module *module, llvm::StringRef alias) const;
  bool isNamedIn(const ImportDecl &importDecl, llvm::StringRef alias) const;
  bool isFromBuiltinModule(const Module *module, llvm::StringRef alias) const;


  Module *unreadyModule(const ExportsFrame &frame);
  Module *stepNeeds(Module *module, const ExportStep &step);
  Module *laterSource(llvm::StringRef name, llvm::StringRef &declared);
  Module *namedExportNeeds(Module *module, const NamedExportStep &step);


  ResultExportTarget findExport(Module *from, llvm::StringRef path, const ModuleName &name);
  ResultExportTarget exportNamed(const llvm::StringMap<ModuleExport> &theirs, Module *from,
                                 llvm::StringRef path, const ModuleName &name);


  std::optional<ModuleBinding> readyLocal(Module *module, llvm::StringRef name);
  std::optional<ModuleBinding> localTarget(Module *module, const ModuleName &name);
  std::optional<ModuleBinding> reexportedTarget(const ExportSource &source, const ModuleName &name);


  ClassDecl *nodeClass(const NodeName &nodeName) const;
  Stmt *exportedType(Module *source, llvm::StringRef name);
  std::vector<ExportStep> exportSteps(Module *module) const;
  std::optional<llvm::StringRef> itemName(const Stmt *item) const;
  ResultSourceUsable exportSource(const ExportSource *source) const;
  const llvm::StringMap<ModuleExport> &readyExports(Module *module);
  ResultWholeImportOk wholeImport(const ImportDecl &importDecl) const;
  ResultImportSource checkImportSource(const ImportDecl &importDecl) const;
  std::optional<ScopeEntry> exportedValue(Module *source, llvm::StringRef name);
  const ModuleExport *exportedAs(const ModuleState &here, llvm::StringRef name) const;
  ResultExportUnique freeExport(const ModuleState &here, const ModuleName &name) const;
  std::optional<ImportedName> findImport(const Module *module, llvm::StringRef name) const;
  ResultLocalIsKnown localExport(const Module *module, const ModuleName &name, bool isFound) const;
  std::string loadLabel(llvm::StringRef name, llvm::StringRef declared, const Module *source) const;
  ResultAliasIsUnused freeName(const ModuleState &here, const ImportDecl &importDecl,
                               const ModuleName &name) const;
  ResultStarIsUnique starExport(llvm::StringRef name, const ModuleExport *ours,
                                const ExportAll *all, const ModuleBinding &target) const;

  SemanticState &state;
  StmtChecker *stmtChecker = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  BuiltinModuleChecker *builtinModuleChecker = nullptr;
};

}
