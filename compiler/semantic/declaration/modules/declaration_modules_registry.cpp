#include "compiler/semantic/declaration/modules/declaration_modules_registry.h"
#include "compiler/program/library/fs/fs_function_table/fs_function_table.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "compiler/program/module/load_chain/load_chain.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker_errors.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

DeclarationModulesRegistry::DeclarationModulesRegistry(SemanticState &owner)
    : state(owner) {}

void DeclarationModulesRegistry::useModule(Module *module) {
  auto &modules = this->state.declaration.modules;
  auto &scopes = this->state.walk.scopes;
  auto *current = modules.getCurrentModule();
  if (module == current) {
    return;
  }
  assert(scopes.isTopLevel() && scopes.isTopLevelTypes() && "a file changes only at the top level");
  if (current != nullptr) {
    auto &left = modules.getModuleState(current);
    left.values = scopes.getTopValues();
    left.types = scopes.getTopTypes();
  }
  modules.rememberCurrentModule(module);
  scopes.replaceTopValues(modules.getModuleState(module).values);
  scopes.replaceTopTypes(modules.getModuleState(module).types);
}

void DeclarationModulesRegistry::addDeclaredItem(ModuleState &here, const Stmt *item) const {
  auto *varDecl = dyn_cast<VarDeclStmt>(item);
  if (varDecl != nullptr) {
    for (const VarDeclarator &declarator : varDecl->decls) {
      this->addDeclared(here, declarator.name, declarator.loc);
      here.topLevelNames.insert(declarator.name);
    }
    return;
  }
  auto name = this->itemName(item);
  if (name.has_value()) {
    this->addDeclared(here, *name, item->loc());
  }
}

void DeclarationModulesRegistry::addDeclared(ModuleState &here, llvm::StringRef name,
                                             llvm::SMLoc loc) const {
  if (!name.empty()) {
    here.declared.try_emplace(name, loc);
  }
}

std::optional<llvm::StringRef> DeclarationModulesRegistry::itemName(const Stmt *item) const {
  auto *functionDecl = dyn_cast<FunctionDecl>(item);
  if (functionDecl != nullptr) {
    return llvm::StringRef(functionDecl->name);
  }
  auto *alias = dyn_cast<TypeAliasDecl>(item);
  if (alias != nullptr) {
    return llvm::StringRef(alias->name);
  }
  auto *enumDecl = dyn_cast<EnumDecl>(item);
  if (enumDecl != nullptr) {
    return llvm::StringRef(enumDecl->name);
  }
  auto *classDecl = dyn_cast<ClassDecl>(item);
  if (classDecl != nullptr) {
    return llvm::StringRef(classDecl->name);
  }

  return std::nullopt;
}

DeclarationModulesRegistry::ResultSourceUsable
DeclarationModulesRegistry::exportSource(const ExportSource *source) const {
  if (source != nullptr && source->path == LibraryModuleNames::kNeverthrowPath) {
    auto message = ErrorTextExportSource::neverthrow();

    return err(DeclarationModulesRegistryExportSourceError{
        .code = DeclarationModulesRegistryExportSourceErrorCode::NEVERTHROW,
        .message = message,
    });
  }
  if (source != nullptr && BuiltinModulePaths::isBuiltinModule(source->path)) {
    auto message = ErrorTextExportSource::builtin(source->path);

    return err(DeclarationModulesRegistryExportSourceError{
        .code = DeclarationModulesRegistryExportSourceErrorCode::BUILTIN_MODULE,
        .message = message,
    });
  }

  return ok(source);
}

DeclarationModulesRegistry::ResultLocalIsKnown
DeclarationModulesRegistry::localExport(const Module *module, const ModuleName &name,
                                        bool isFound) const {
  if (isFound) {
    return ok(&name);
  }
  if (this->isFromBuiltinModule(module, name.name)) {
    auto message = ErrorTextLocalExport::builtin(name.name);

    return err(DeclarationModulesRegistryLocalExportError{
        .code = DeclarationModulesRegistryLocalExportErrorCode::FROM_BUILTIN_MODULE,
        .message = message,
    });
  }
  if (!this->isImportedByName(module, name.name)) {
    auto message = ErrorTextLocalExport::notFound(name.name);

    return err(DeclarationModulesRegistryLocalExportError{
        .code = DeclarationModulesRegistryLocalExportErrorCode::NOT_FOUND,
        .message = message,
    });
  }

  return err(DeclarationModulesRegistryLocalExportError{
      .code = DeclarationModulesRegistryLocalExportErrorCode::ALREADY_REPORTED,
      .message = "",
  });
}

bool DeclarationModulesRegistry::isNamedIn(const ImportDecl &importDecl,
                                           llvm::StringRef alias) const {
  for (const ModuleName &imported : importDecl.names) {
    if (imported.as == alias) {
      return true;
    }
  }

  return false;
}

bool DeclarationModulesRegistry::isImportedByName(const Module *module,
                                                  llvm::StringRef alias) const {
  for (const ImportDecl &importDecl : module->file.imports) {
    if (this->isNamedIn(importDecl, alias)) {
      return true;
    }
  }

  return false;
}

bool DeclarationModulesRegistry::isFromBuiltinModule(const Module *module,
                                                     llvm::StringRef alias) const {
  for (const ImportDecl &importDecl : module->file.imports) {
    bool isBringing = importDecl.whole == alias || this->isNamedIn(importDecl, alias);
    if (isBringing && BuiltinModulePaths::isBuiltinModule(importDecl.path)) {
      return true;
    }
  }

  return false;
}

DeclarationModulesRegistry::ResultExportUnique
DeclarationModulesRegistry::freeExport(const ModuleState &here, const ModuleName &name) const {
  if (here.exports.contains(name.as)) {
    auto message = ErrorTextFreeExport::duplicate(name.as);

    return err(DeclarationModulesRegistryFreeExportError{
        .code = DeclarationModulesRegistryFreeExportErrorCode::DUPLICATE_EXPORT,
        .message = message,
    });
  }

  return ok(&name);
}

void DeclarationModulesRegistry::exportName(Module *module, const NamedExportStep &step) {
  auto &here = this->state.declaration.modules.getModuleState(module);
  const ExportSource *source = step.source;
  const ModuleName &name = *step.name;
  if (source != nullptr && source->from == nullptr) {
    return;
  }
  auto target =
      source != nullptr ? this->reexportedTarget(*source, name) : this->localTarget(module, name);
  if (!target.has_value()) {
    return;
  }
  auto unique = this->freeExport(here, name);
  if (unique.isErr()) {
    llvm::SMLoc exportedAt = here.exports.find(name.as)->second.loc;
    auto previous = ErrorTextFreeExport::previous();
    this->state.diagnostics.error(name.asLoc, unique.error().message);
    this->state.diagnostics.note(exportedAt, previous);
    return;
  }
  auto exported = ModuleExport{.target = *target, .loc = name.asLoc, .all = nullptr};
  here.exports.try_emplace(name.as, exported);
}

std::optional<ModuleBinding>
DeclarationModulesRegistry::reexportedTarget(const ExportSource &source, const ModuleName &name) {
  const auto &theirs = this->readyExports(source.from);
  auto found = this->exportNamed(theirs, source.from, source.path, name);
  if (found.isErr()) {
    this->state.diagnostics.error(name.loc, found.error().message);

    return std::nullopt;
  }

  return found.value();
}

std::optional<ModuleBinding> DeclarationModulesRegistry::localTarget(Module *module,
                                                                     const ModuleName &name) {
  auto target = this->readyLocal(module, name.name);
  auto known = this->localExport(module, name, target.has_value());
  if (known.isOk()) {
    return target;
  }
  if (known.error().code != DeclarationModulesRegistryLocalExportErrorCode::ALREADY_REPORTED) {
    this->state.diagnostics.error(name.loc, known.error().message);
  }

  return std::nullopt;
}

DeclarationModulesRegistry::ResultStarIsUnique
DeclarationModulesRegistry::starExport(llvm::StringRef name, const ModuleExport *ours,
                                       const ExportAll *all, const ModuleBinding &target) const {
  bool isClash = ours != nullptr && ours->all != nullptr && !(ours->target == target);
  if (isClash) {
    auto message = ErrorTextStarExport::twice(name, ours->all->source.path, all->source.path);

    return err(DeclarationModulesRegistryStarExportError{
        .code = DeclarationModulesRegistryStarExportErrorCode::EXPORTED_BY_TWO,
        .message = message,
    });
  }

  return ok(all);
}

const llvm::StringMap<ModuleExport> &DeclarationModulesRegistry::exportsOf(Module *module) {
  auto &asked = this->state.declaration.modules.getModuleState(module);
  if (asked.exportsDone || asked.exportsBusy) {
    return asked.exports;
  }
  auto frames = std::vector<ExportsFrame>();
  this->startExports(frames, module);
  while (!frames.empty()) {
    auto *needed = this->unreadyModule(frames.back());
    if (needed != nullptr) {
      this->startExports(frames, needed);
    } else {
      this->runExportStep(frames);
    }
  }

  return asked.exports;
}

void DeclarationModulesRegistry::startExports(std::vector<ExportsFrame> &frames, Module *module) {
  this->state.declaration.modules.getModuleState(module).exportsBusy = true;
  auto steps = this->exportSteps(module);
  auto frame = ExportsFrame{.module = module, .steps = std::move(steps), .nextStep = 0};
  frames.push_back(std::move(frame));
}

std::vector<ExportStep> DeclarationModulesRegistry::exportSteps(Module *module) const {
  auto steps = std::vector<ExportStep>();
  for (const ExportDecl &exportDecl : module->file.exports) {
    auto usable = this->exportSource(exportDecl.source());
    if (usable.isOk()) {
      this->addNameSteps(exportDecl, steps);
    } else {
      llvm::SMLoc loc = exportDecl.source()->pathLoc;
      steps.push_back(BadSourceStep{.loc = loc, .message = usable.error().message});
    }
  }
  for (const ExportDecl &exportDecl : module->file.exports) {
    const ExportAll *all = exportDecl.form.as<ExportAll>();
    bool isFromOther = all != nullptr && all->source.from != nullptr && all->source.from != module;
    if (isFromOther) {
      steps.push_back(StarExportStep{.exportDecl = &exportDecl, .from = all->source.from});
    }
  }

  return steps;
}

void DeclarationModulesRegistry::addNameSteps(const ExportDecl &exportDecl,
                                              std::vector<ExportStep> &steps) const {
  for (const ModuleName &name : exportDecl.names()) {
    steps.push_back(NamedExportStep{.source = exportDecl.source(), .name = &name});
  }
}

Module *DeclarationModulesRegistry::unreadyModule(const ExportsFrame &frame) {
  if (frame.nextStep == frame.steps.size()) {
    return nullptr;
  }
  auto *needed = this->stepNeeds(frame.module, frame.steps[frame.nextStep]);
  if (needed == nullptr) {
    return nullptr;
  }
  const auto &neededState = this->state.declaration.modules.getModuleState(needed);
  bool isWaiting = !neededState.exportsDone && !neededState.exportsBusy;

  return isWaiting ? needed : nullptr;
}

Module *DeclarationModulesRegistry::stepNeeds(Module *module, const ExportStep &step) {
  switch (step.kind()) {
    case ExportStepKind::BAD_SOURCE:
      return nullptr;
    case ExportStepKind::NAMED:
      return this->namedExportNeeds(module, *step.as<NamedExportStep>());
    case ExportStepKind::STAR:
      return step.as<StarExportStep>()->from;
  }
  llvm_unreachable("unknown export step");
}

Module *DeclarationModulesRegistry::namedExportNeeds(Module *module, const NamedExportStep &step) {
  if (step.source != nullptr) {
    return step.source->from;
  }
  const auto &here = this->state.declaration.modules.getModuleState(module);
  if (here.declared.contains(step.name->name)) {
    return nullptr;
  }
  auto imported = this->findImport(module, step.name->name);

  return imported.has_value() ? imported->from : nullptr;
}

void DeclarationModulesRegistry::runExportStep(std::vector<ExportsFrame> &frames) {
  ExportsFrame &frame = frames.back();
  if (frame.nextStep == frame.steps.size()) {
    return this->finishExports(frames);
  }
  const ExportStep &step = frame.steps[frame.nextStep];
  frame.nextStep += 1;

  return this->applyExportStep(frame.module, step);
}

void DeclarationModulesRegistry::applyExportStep(Module *module, const ExportStep &step) {
  switch (step.kind()) {
    case ExportStepKind::BAD_SOURCE:
      return this->reportBadSource(*step.as<BadSourceStep>());
    case ExportStepKind::NAMED:
      return this->exportName(module, *step.as<NamedExportStep>());
    case ExportStepKind::STAR:
      return this->mergeStarExport(module, *step.as<StarExportStep>());
  }
}

void DeclarationModulesRegistry::reportBadSource(const BadSourceStep &step) {
  this->state.diagnostics.error(step.loc, step.message);
}

void DeclarationModulesRegistry::mergeStarExport(Module *module, const StarExportStep &star) {
  auto &here = this->state.declaration.modules.getModuleState(module);
  const ExportDecl &exportDecl = *star.exportDecl;
  const ExportAll *all = exportDecl.form.as<ExportAll>();
  for (const auto &entry : this->readyExports(star.from)) {
    llvm::StringRef name = entry.getKey();
    const ModuleBinding &target = entry.getValue().target;
    const ModuleExport *ours = this->exportedAs(here, name);
    auto unique = this->starExport(name, ours, all, target);
    if (unique.isErr()) {
      this->state.diagnostics.error(exportDecl.loc, unique.error().message);
    }
    auto exported = ModuleExport{.target = target, .loc = exportDecl.loc, .all = all};
    here.exports.try_emplace(name, exported);
  }
}

const ModuleExport *DeclarationModulesRegistry::exportedAs(const ModuleState &here,
                                                           llvm::StringRef name) const {
  auto existing = here.exports.find(name);
  if (existing == here.exports.end()) {
    return nullptr;
  }

  return &existing->second;
}

void DeclarationModulesRegistry::finishExports(std::vector<ExportsFrame> &frames) {
  auto &done = this->state.declaration.modules.getModuleState(frames.back().module);
  done.exportsBusy = false;
  done.exportsDone = true;
  frames.pop_back();
}

DeclarationModulesRegistry::ResultExportTarget
DeclarationModulesRegistry::findExport(Module *from, llvm::StringRef path, const ModuleName &name) {
  const auto &theirs = this->exportsOf(from);

  return this->exportNamed(theirs, from, path, name);
}

const llvm::StringMap<ModuleExport> &DeclarationModulesRegistry::readyExports(Module *module) {
  return this->state.declaration.modules.getModuleState(module).exports;
}

DeclarationModulesRegistry::ResultExportTarget
DeclarationModulesRegistry::exportNamed(const llvm::StringMap<ModuleExport> &theirs, Module *from,
                                        llvm::StringRef path, const ModuleName &name) {
  auto found = theirs.find(name.name);
  bool isMissing = found == theirs.end();
  if (isMissing && from->builtin) {
    auto hint = ErrorTextEventsExport::hint(name.name);
    auto message = ErrorTextFindExport::library(path, name.name, hint);

    return err(DeclarationModulesRegistryFindExportError{
        .code = DeclarationModulesRegistryFindExportErrorCode::NOT_EXPORTED_BY_LIBRARY,
        .message = message,
    });
  }
  const auto &theirState = this->state.declaration.modules.getModuleState(from);
  if (isMissing && theirState.declared.contains(name.name)) {
    auto message = ErrorTextFindExport::unmarked(path, name.name, from->name);

    return err(DeclarationModulesRegistryFindExportError{
        .code = DeclarationModulesRegistryFindExportErrorCode::DECLARED_WITHOUT_EXPORT,
        .message = message,
    });
  }
  if (isMissing) {
    auto message = ErrorTextFindExport::missing(path, name.name);

    return err(DeclarationModulesRegistryFindExportError{
        .code = DeclarationModulesRegistryFindExportErrorCode::NOT_EXPORTED,
        .message = message,
    });
  }

  return ok(found->second.target);
}

std::optional<ModuleBinding> DeclarationModulesRegistry::readyLocal(Module *module,
                                                                    llvm::StringRef name) {
  if (this->state.declaration.modules.getModuleState(module).declared.contains(name)) {
    return ModuleBinding{.module = module, .name = name};
  }
  auto imported = this->findImport(module, name);
  if (!imported.has_value() || imported->from == nullptr) {
    return std::nullopt;
  }
  const auto &theirs = this->readyExports(imported->from);
  auto found = theirs.find(imported->name);
  if (found == theirs.end()) {
    return std::nullopt;
  }

  return found->second.target;
}

std::optional<ImportedName> DeclarationModulesRegistry::findImport(const Module *module,
                                                                   llvm::StringRef name) const {
  for (const ImportDecl &importDecl : module->file.imports) {
    for (const ModuleName &imported : importDecl.names) {
      if (imported.as == name) {
        return ImportedName{.from = importDecl.from, .name = imported.name};
      }
    }
  }

  return std::nullopt;
}

DeclarationModulesRegistry::ResultWholeImportOk
DeclarationModulesRegistry::wholeImport(const ImportDecl &importDecl) const {
  bool isWhole =
      !importDecl.whole.empty() && importDecl.from != nullptr && importDecl.from->builtin;
  if (isWhole && importDecl.wholeIsDefault) {
    auto message = ErrorTextWholeImport::byDefault(importDecl.path);

    return err(DeclarationModulesRegistryWholeImportError{
        .code = DeclarationModulesRegistryWholeImportErrorCode::DEFAULT_IMPORT,
        .message = message,
    });
  }
  if (isWhole) {
    auto message = ErrorTextWholeImport::star(importDecl.path);

    return err(DeclarationModulesRegistryWholeImportError{
        .code = DeclarationModulesRegistryWholeImportErrorCode::STAR_IMPORT,
        .message = message,
    });
  }

  return ok(&importDecl);
}

DeclarationModulesRegistry::ResultAliasIsUnused
DeclarationModulesRegistry::freeName(const ModuleState &here, const ImportDecl &importDecl,
                                     const ModuleName &name) const {
  if (here.declared.contains(name.as)) {
    auto message = ErrorTextFreeName::conflicts(name.as, name.name, importDecl.path);

    return err(DeclarationModulesRegistryFreeNameError{
        .code = DeclarationModulesRegistryFreeNameErrorCode::DECLARED_IN_FILE,
        .message = message,
    });
  }
  if (here.nodeNames.contains(name.as)) {
    auto message = ErrorTextFreeName::duplicate(name.as);

    return err(DeclarationModulesRegistryFreeNameError{
        .code = DeclarationModulesRegistryFreeNameErrorCode::IMPORTED_FROM_NODE,
        .message = message,
    });
  }
  if (here.imports.contains(name.as)) {
    auto message = ErrorTextFreeName::duplicate(name.as);

    return err(DeclarationModulesRegistryFreeNameError{
        .code = DeclarationModulesRegistryFreeNameErrorCode::IMPORTED_TWICE,
        .message = message,
    });
  }

  return ok(&name);
}

void DeclarationModulesRegistry::reportTaken(const ModuleState &here, const ModuleName &name,
                                             const DeclarationModulesRegistryFreeNameError &error) {
  this->state.diagnostics.error(name.asLoc, error.message);
  switch (error.code) {
    case DeclarationModulesRegistryFreeNameErrorCode::DECLARED_IN_FILE: {
      llvm::SMLoc declaredAt = here.declared.find(name.as)->second;
      auto declared = ErrorTextFreeName::declared(name.as);
      this->state.diagnostics.note(declaredAt, declared);
      break;
    }
    case DeclarationModulesRegistryFreeNameErrorCode::IMPORTED_FROM_NODE:
      break;
    case DeclarationModulesRegistryFreeNameErrorCode::IMPORTED_TWICE: {
      llvm::SMLoc importedAt = here.imports.find(name.as)->second.loc;
      auto previous = ErrorTextFreeName::previous();
      this->state.diagnostics.note(importedAt, previous);
      break;
    }
  }
}

void DeclarationModulesRegistry::importName(ModuleState &here, const ImportDecl &importDecl,
                                            const ModuleName &name) {
  if (importDecl.from == nullptr) {
    here.badImports.insert(name.as);
    return;
  }
  auto target = this->findExport(importDecl.from, importDecl.path, name);
  if (target.isErr()) {
    this->state.diagnostics.error(name.loc, target.error().message);
    here.badImports.insert(name.as);
    return;
  }
  auto unused = this->freeName(here, importDecl, name);
  if (unused.isErr()) {
    this->reportTaken(here, name, unused.error());
    return;
  }
  auto imported = ModuleImport{.target = target.value(), .loc = name.asLoc};
  here.imports.try_emplace(name.as, imported);
}

void DeclarationModulesRegistry::resolveImports(Module *module) {
  for (const ImportDecl &importDecl : llvm::reverse(module->file.imports)) {
    this->state.walk.tasks.pushTask(ResolveImport{.module = module, .importDecl = &importDecl});
  }
}

void DeclarationModulesRegistry::resolveImport(const ResolveImport &task) {
  Module *module = task.module;
  const ImportDecl &importDecl = *task.importDecl;
  auto &here = this->state.declaration.modules.getModuleState(module);
  auto source = this->checkImportSource(importDecl);
  if (source.isErr()) {
    this->state.diagnostics.error(importDecl.pathLoc, source.error().message);
    for (const ModuleName &name : importDecl.names) {
      here.badImports.insert(name.as);
    }
    return;
  }
  if (BuiltinModulePaths::isBuiltinModule(importDecl.path) && importDecl.from == nullptr) {
    return this->state.walk.tasks.pushTask(
        ImportBuiltinModule{.module = module, .importDecl = &importDecl});
  }
  auto whole = this->wholeImport(importDecl);
  if (whole.isErr()) {
    this->state.diagnostics.error(importDecl.wholeLoc, whole.error().message);
    here.badImports.insert(importDecl.whole);
  }
  for (const ModuleName &name : importDecl.names) {
    this->importName(here, importDecl, name);
  }
}

void DeclarationModulesRegistry::bindImports() {
  auto *current = this->state.declaration.modules.getCurrentModule();
  auto &here = this->state.declaration.modules.getModuleState(current);
  for (const auto &entry : here.nodeNames) {
    auto *classDecl = this->nodeClass(entry.getValue());
    if (classDecl != nullptr) {
      auto value = ScopeEntry{ClassEntry{.cls = classDecl}};
      this->state.walk.scopes.addTopType(entry.getKey(), classDecl);
      this->state.walk.scopes.addTopValue(entry.getKey(), value);
    }
  }
  for (const auto &entry : here.imports) {
    this->bindImportedType(entry.getKey(), entry.getValue().target);
    this->bindImportedValue(here, entry.getKey(), entry.getValue().target);
  }
}

ClassDecl *DeclarationModulesRegistry::nodeClass(const NodeName &nodeName) const {
  if (nodeName.module != NodeModule::Fs) {
    return nullptr;
  }
  auto found = FsFunctionTable::find(nodeName.name);
  if (found == FsFunction::STATS) {
    return this->state.library.filesystem.getStatsClass();
  }
  if (found == FsFunction::DIRENT) {
    return this->state.library.filesystem.getDirentClass();
  }

  return nullptr;
}

void DeclarationModulesRegistry::bindImportedType(llvm::StringRef alias,
                                                  const ModuleBinding &target) {
  auto &scopes = this->state.walk.scopes;
  if (scopes.hasTopType(alias)) {
    return;
  }
  auto *type = this->exportedType(target.module, target.name);
  if (type != nullptr) {
    scopes.addTopType(alias, type);
  }
}

void DeclarationModulesRegistry::bindImportedValue(ModuleState &here, llvm::StringRef alias,
                                                   const ModuleBinding &target) {
  auto &scopes = this->state.walk.scopes;
  if (scopes.hasTopValue(alias)) {
    return;
  }
  auto found = this->exportedValue(target.module, target.name);
  if (!found.has_value() || !found->isFound()) {
    return;
  }
  scopes.addTopValue(alias, *found);
  bool isOwn = target.module == this->state.declaration.modules.getCurrentModule();
  if (found->var() != nullptr && !isOwn) {
    here.importedVars.insert(found->var());
  }
}

void DeclarationModulesRegistry::enterModule(Module *module) {
  this->useModule(module);
  this->bindImports();
}

void DeclarationModulesRegistry::enterHomeModule(const FunctionDecl *functionDecl) {
  auto *home = this->state.declaration.modules.findDeclModule(functionDecl);
  if (home != nullptr) {
    this->enterModule(home);
  }
}

void DeclarationModulesRegistry::bindAllImports(const std::vector<Module *> &modules) {
  for (Module *module : modules) {
    this->enterModule(module);
  }
}

Stmt *DeclarationModulesRegistry::exportedType(Module *source, llvm::StringRef name) {
  if (source == this->state.declaration.modules.getCurrentModule()) {
    return this->state.walk.scopes.findTopType(name);
  }

  return this->state.declaration.modules.getModuleState(source).types.lookup(name);
}

std::optional<ScopeEntry> DeclarationModulesRegistry::exportedValue(Module *source,
                                                                    llvm::StringRef name) {
  if (source == this->state.declaration.modules.getCurrentModule()) {
    return this->state.walk.scopes.findTopValue(name);
  }
  const llvm::StringMap<ScopeEntry> &values =
      this->state.declaration.modules.getModuleState(source).values;
  auto found = values.find(name);
  if (found == values.end()) {
    return std::nullopt;
  }

  return found->second;
}

void DeclarationModulesRegistry::noteImportsInFunctions(SourceFile &program) {
  for (Module *module : program.modules) {
    for (const auto &entry : this->state.declaration.modules.getModuleState(module).imports) {
      if (this->state.declaration.mentions.isUsedInFunction(entry.getKey())) {
        this->state.declaration.mentions.markUsedInFunction(entry.getValue().target.name);
      }
    }
  }
}

FunctionDecl *DeclarationModulesRegistry::cycleCallee(FunctionDecl *functionDecl,
                                                      llvm::StringRef name) {
  auto *home = this->state.declaration.modules.findDeclModule(functionDecl);
  if (home == nullptr) {
    return nullptr;
  }
  auto found = this->exportedValue(home, name);
  if (!found.has_value() || found->func() == nullptr) {
    return nullptr;
  }

  return found->func()->decl;
}

void DeclarationModulesRegistry::startTopLevel(const StartTopLevel &task) {
  Module *module = task.module;
  auto &here = this->state.declaration.modules.getModuleState(module);
  auto *outer = this->state.declaration.modules.getCurrentModule();
  this->useModule(module);
  this->state.declaration.modules.enterLoading(module);
  here.busy = true;
  this->bindImports();
  if (here.flow) {
    this->state.walk.flow.replaceFlow(std::move(*here.flow));
    here.flow.reset();
  }
  this->state.walk.tasks.pushTask(FinishTopLevel{.module = module, .outer = outer});
  this->state.walk.tasks.pushTask(NextTopLevelItem{.module = module, .until = task.until});
}

void DeclarationModulesRegistry::nextTopLevelItem(const NextTopLevelItem &task) {
  Module *module = task.module;
  auto &here = this->state.declaration.modules.getModuleState(module);
  std::vector<Stmt *> &items = module->file.items;
  if (here.next >= items.size()) {
    return;
  }
  Stmt *item = items[here.next++];
  if (isa<FunctionDecl>(item)) {
    return this->state.walk.tasks.pushTask(task);
  }
  this->state.walk.block.rememberBlockPosition(items, here.next - 1);
  if (!this->isDeclaringName(item, task.until)) {
    this->state.walk.tasks.pushTask(task);
  }
  this->state.walk.tasks.pushTask(WalkStmt{.stmt = item});
}

bool DeclarationModulesRegistry::isDeclaringName(const Stmt *item, llvm::StringRef name) const {
  auto *varDecl = dyn_cast<VarDeclStmt>(item);
  if (name.empty() || varDecl == nullptr) {
    return false;
  }
  for (const VarDeclarator &declarator : varDecl->decls) {
    if (declarator.name == name) {
      return true;
    }
  }

  return false;
}

void DeclarationModulesRegistry::finishTopLevel(const FinishTopLevel &task) {
  Module *module = task.module;
  auto &here = this->state.declaration.modules.getModuleState(module);
  auto close = CloseTopLevel{.module = module, .outer = task.outer};
  if (here.next != module->file.items.size()) {
    here.flow = this->state.walk.flow.getFlow();

    return this->closeTopLevel(close);
  }
  here.loaded = true;
  this->state.walk.tasks.pushTask(close);
  this->state.walk.tasks.pushTask(ReportUnlistenedErrors{});
}

void DeclarationModulesRegistry::closeTopLevel(const CloseTopLevel &task) {
  auto &here = this->state.declaration.modules.getModuleState(task.module);
  here.busy = false;
  this->state.declaration.modules.leaveLoading();
  here.values = this->state.walk.scopes.getTopValues();
  here.types = this->state.walk.scopes.getTopTypes();
  if (task.outer != nullptr) {
    this->useModule(task.outer);
  }
}

Module *DeclarationModulesRegistry::laterSource(llvm::StringRef name,
                                                llvm::StringRef &declared) const {
  auto &here = this->state.declaration.modules.getModuleState(
      this->state.declaration.modules.getCurrentModule());
  declared = name;
  auto found = here.imports.find(name);
  if (found != here.imports.end()) {
    declared = found->second.target.name;
    Module *source = found->second.target.module;
    return this->state.declaration.modules.getModuleState(source).loaded ? nullptr : source;
  }
  if (this->state.declaration.modules.getLoadingModule() != nullptr &&
      this->state.declaration.modules.getCurrentModule() !=
          this->state.declaration.modules.getLoadingModule() &&
      !here.loaded && here.topLevelNames.contains(name)) {
    return this->state.declaration.modules.getCurrentModule();
  }

  return nullptr;
}

std::optional<StartLoadEarly>
DeclarationModulesRegistry::findEarlyLoad(llvm::StringRef name) const {
  auto entry = this->state.walk.scopes.findName(name);
  auto *loading = this->state.declaration.modules.getLoadingModule();
  if (entry.isFound() || loading == nullptr) {
    return std::nullopt;
  }
  llvm::StringRef declared;
  auto *source = this->laterSource(name, declared);
  bool isLoadingHere = !this->state.walk.functions.isInsideFunction() &&
                       this->state.declaration.modules.getCurrentModule() == loading;
  if (source == nullptr || isLoadingHere || !this->isLoadable(source, declared)) {
    return std::nullopt;
  }

  return StartLoadEarly{.source = source, .name = declared};
}

bool DeclarationModulesRegistry::isLoadable(Module *source, llvm::StringRef name) const {
  const auto &there = this->state.declaration.modules.getModuleState(source);

  return !there.loaded && !there.busy && there.topLevelNames.contains(name);
}

void DeclarationModulesRegistry::startLoadEarly(const StartLoadEarly &task) {
  this->state.walk.tasks.pushTask(LoadEarlyBody{.source = task.source, .name = task.name});
  this->state.walk.tasks.pushTask(EnterLoadBody{});
}

void DeclarationModulesRegistry::loadEarlyBody(const LoadEarlyBody &task) {
  this->state.walk.functions.forgetEarlyFunction();
  this->state.code.variables.enterModuleLoad();
  this->state.code.concatenation.enterModuleLoad();
  this->state.inference.returns.enterModuleLoad();
  this->state.diagnostics.pushList(DiagnosticRoute::PROGRAM);
  this->state.walk.tasks.pushTask(FinishLoadEarly{.source = task.source});
  this->state.walk.tasks.pushTask(StartTopLevel{.module = task.source, .until = task.name});
}

void DeclarationModulesRegistry::finishLoadEarly(const FinishLoadEarly &task) {
  this->state.diagnostics.popList();
  this->state.inference.returns.leaveModuleLoad();
  this->state.code.concatenation.leaveModuleLoad();
  this->state.code.variables.leaveModuleLoad();
  this->state.walk.tasks.pushTask(BindLoadedNames{.source = task.source});
  this->state.walk.tasks.pushTask(LeaveLoadBody{});
}

void DeclarationModulesRegistry::bindLoadedNames(const BindLoadedNames &task) {
  auto *current = this->state.declaration.modules.getCurrentModule();
  if (task.source != current) {
    return this->bindImports();
  }
  for (const auto &value : this->state.declaration.modules.getModuleState(current).values) {
    this->state.walk.scopes.addTopValue(value.getKey(), value.getValue());
  }
}

std::string DeclarationModulesRegistry::loadLabel(llvm::StringRef name, llvm::StringRef declared,
                                                  const Module *source) const {
  if (declared != name) {
    return ErrorTextNotLoaded::label(name, declared, source->name);
  }
  if (source != this->state.declaration.modules.getLoadingModule()) {
    return ErrorTextNotLoaded::labelOf(name, source->name);
  }

  return ErrorTextNotLoaded::quoted(name);
}

DeclarationModulesRegistry::ResultSourceIsLoaded
DeclarationModulesRegistry::checkLoaded(const NameExpr *name) {
  if (this->state.declaration.modules
          .getModuleState(this->state.declaration.modules.getCurrentModule())
          .badImports.contains(name->name)) {
    return err(DeclarationModulesRegistryCheckLoadedError{
        .code = DeclarationModulesRegistryCheckLoadedErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }

  llvm::StringRef declared;
  auto *source = this->laterSource(name->name, declared);
  Module *reader = this->state.declaration.modules.getLoadingModule() != nullptr
                       ? this->state.declaration.modules.getLoadingModule()
                       : this->state.declaration.modules.getCurrentModule();
  std::string what = source != nullptr ? this->loadLabel(name->name, declared, source) : "";
  bool isTopLevel = !this->state.walk.functions.isInsideFunction() &&
                    this->state.declaration.modules.getCurrentModule() ==
                        this->state.declaration.modules.getLoadingModule();
  if (source != nullptr && isTopLevel) {
    auto chain = LoadChain::describe(reader, source);
    auto message =
        ErrorTextNotLoaded::whileLoads(reader->name, what, source->name, chain, declared);

    return err(DeclarationModulesRegistryCheckLoadedError{
        .code = DeclarationModulesRegistryCheckLoadedErrorCode::READ_WHILE_LOADING,
        .message = message,
    });
  }
  if (source != nullptr && this->state.walk.functions.getEarlyFunction() != nullptr) {
    auto message = ErrorTextNotLoaded::beforeCall(
        what, reader->name, this->state.walk.functions.getEarlyFunction()->name);

    return err(DeclarationModulesRegistryCheckLoadedError{
        .code = DeclarationModulesRegistryCheckLoadedErrorCode::NO_TYPE_BEFORE_CALL,
        .message = message,
    });
  }
  if (source != nullptr) {
    auto message = ErrorTextNotLoaded::inFunction(what, reader->name);

    return err(DeclarationModulesRegistryCheckLoadedError{
        .code = DeclarationModulesRegistryCheckLoadedErrorCode::NO_TYPE_IN_FUNCTION,
        .message = message,
    });
  }

  return ok(name);
}

void DeclarationModulesRegistry::declareNames(Module *module) {
  auto &here = this->state.declaration.modules.getModuleState(module);
  for (Stmt *item : module->file.items) {
    this->addDeclaredItem(here, item);
  }
}

DeclarationModulesRegistry::ResultImportSource
DeclarationModulesRegistry::checkImportSource(const ImportDecl &importDecl) const {
  if (importDecl.path == LibraryModuleNames::kNeverthrowPath) {
    auto message = ErrorTextImportSource::neverthrow();

    return err(DeclarationModulesRegistryImportSourceError{
        .code = DeclarationModulesRegistryImportSourceErrorCode::NEVERTHROW_IMPORT,
        .message = message,
    });
  }

  return ok(&importDecl);
}

}
