#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/declare/module_declarer/module_declarer_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/union/union/union.h"

namespace tz {

class SemanticParts;
class ClassLinks;
class LateClasses;
class ImportBinder;
class TypeDeclarer;
class ClassDeclarer;
class FunctionChecker;
class GenericClassInstantiator;

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
  const ExportDecl *decl;
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

class ModuleDeclarer {
public:
  using ResultExportTarget = Result<ModuleBinding, ModuleDeclarerFindExportError>;

  ModuleDeclarer(SemanticState &owner, SemanticParts &all);

  void useModule(Module *module);
  void declareModules(SourceFile &program);
  const llvm::StringMap<ModuleExport> &exportsOf(Module *module);
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

  static std::vector<ExportStep> exportSteps(Module *module);
  static void addNameSteps(const ExportDecl &decl, std::vector<ExportStep> &steps);
  static std::optional<ImportedName> findImport(const Module *module, llvm::StringRef name);

  void startExports(std::vector<ExportsFrame> &frames, Module *module);
  Module *unreadyModule(const ExportsFrame &frame);
  Module *stepNeeds(Module *module, const ExportStep &step);
  Module *namedExportNeeds(Module *module, const NamedExportStep &step);
  void runExportStep(std::vector<ExportsFrame> &frames);
  void applyExportStep(Module *module, const ExportStep &step);
  void reportBadSource(const BadSourceStep &step);
  void exportName(Module *module, const NamedExportStep &step);
  void mergeStarExport(Module *module, const StarExportStep &star);
  void finishExports(std::vector<ExportsFrame> &frames);
  const llvm::StringMap<ModuleExport> &readyExports(Module *module);
  ResultExportTarget exportNamed(const llvm::StringMap<ModuleExport> &theirs, Module *from,
                                 llvm::StringRef path, const ModuleName &name);
  std::optional<ModuleBinding> readyLocal(Module *module, llvm::StringRef name);

  SemanticState &state;
  ClassLinks *classLinks = nullptr;
  LateClasses *lateClasses = nullptr;
  ImportBinder *importBinder = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  FunctionChecker *functionChecker = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
