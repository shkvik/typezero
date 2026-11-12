#include "compiler/semantic/declare/module_declarer/module_declarer.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/import_binder/import_binder.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/semantic/infer/late_classes/late_classes.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker_errors.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>

using llvm::dyn_cast;

namespace tz {

ModuleDeclarer::ModuleDeclarer(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->classLinks = &all.classLinks;
  this->lateClasses = &all.lateClasses;
  this->importBinder = &all.importBinder;
  this->typeDeclarer = &all.typeDeclarer;
  this->classDeclarer = &all.classDeclarer;
  this->functionChecker = &all.functionChecker;
  this->genericClassInstantiator = &all.genericClassInstantiator;
}

void ModuleDeclarer::useModule(Module *module) {
  auto &modules = this->state.getModuleStates();
  auto &scopes = this->state.getScopeStack();
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

void ModuleDeclarer::declareModules(SourceFile &program) {
  this->state.getModuleStates().prepareModules(int(program.modules.size()));
  for (Module *module : program.modules) {
    auto &here = this->state.getModuleStates().getModuleState(module);
    for (Stmt *item : module->file.items) {
      this->addDeclaredItem(here, item);
    }
  }
  auto types = this->declareModuleTypes(program);
  for (Module *module : program.modules) {
    this->importBinder->resolveImports(module);
  }
  for (Module *module : program.modules) {
    this->useModule(module);
    this->importBinder->bindImports();
  }
  for (Module *module : program.modules) {
    this->useModule(module);
    this->typeDeclarer->resolveTypes(types[module->order]);
  }
  auto functions = std::vector<FunctionDecl *>();
  for (Module *module : program.modules) {
    this->useModule(module);
    this->declareModuleFunctions(module, functions);
  }
  this->importBinder->bindAllImports(program.modules);
  this->state.getClassStore().markMethodsDeclared();
  this->fillUnfilledInstances();
  this->lateClasses->markLateOverrides(program);
  this->classLinks->linkClasses();
  for (Module *module : program.modules) {
    this->exportsOf(module);
  }
  this->state.getReturnInference().rememberDeclaredFunctions(std::move(functions));
}

void ModuleDeclarer::addDeclaredItem(ModuleState &here, const Stmt *item) const {
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

void ModuleDeclarer::addDeclared(ModuleState &here, llvm::StringRef name, llvm::SMLoc loc) const {
  if (!name.empty()) {
    here.declared.try_emplace(name, loc);
  }
}

std::optional<llvm::StringRef> ModuleDeclarer::itemName(const Stmt *item) const {
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

std::vector<std::vector<Stmt *>> ModuleDeclarer::declareModuleTypes(SourceFile &program) {
  std::vector<std::vector<Stmt *>> types(program.modules.size());
  for (Module *module : program.modules) {
    this->useModule(module);
    types[module->order] = this->typeDeclarer->declareTypeNames(module->file.items);
    for (Stmt *type : types[module->order]) {
      this->state.getModuleStates().rememberDeclModule(type, module);
    }
  }

  return types;
}

void ModuleDeclarer::declareModuleFunctions(Module *module,
                                            std::vector<FunctionDecl *> &functions) {
  for (Stmt *item : module->file.items) {
    auto *functionDecl = dyn_cast<FunctionDecl>(item);
    auto *classDecl = dyn_cast<ClassDecl>(item);
    if (functionDecl != nullptr) {
      this->functionChecker->declareFunction(functionDecl);
      this->state.getModuleStates().rememberDeclModule(functionDecl, module);
      functions.push_back(functionDecl);
    }
    if (classDecl != nullptr && this->isDeclaredClass(classDecl)) {
      this->declareClassFunctions(module, classDecl, functions);
    }
  }
}

bool ModuleDeclarer::isDeclaredClass(const ClassDecl *classDecl) const {
  return classDecl->type != nullptr && classDecl->baseExpr == nullptr;
}

void ModuleDeclarer::declareClassFunctions(Module *module, ClassDecl *classDecl,
                                           std::vector<FunctionDecl *> &functions) {
  this->classDeclarer->declareMethods(classDecl);
  for (FunctionDecl *method : this->classDeclarer->memberFunctions(classDecl)) {
    this->state.getModuleStates().rememberDeclModule(method, module);
    functions.push_back(method);
  }
}

void ModuleDeclarer::fillUnfilledInstances() {
  auto &generics = this->state.getGenericStore();
  for (ClassDecl *instance : generics.takeUnfilledClasses()) {
    generics.enterFilling(instance);
    this->genericClassInstantiator->fillInstance(instance);
    generics.leaveFilling();
  }
}

ModuleDeclarer::ResultSourceUsable ModuleDeclarer::exportSource(const ExportSource *source) const {
  if (source != nullptr && source->path == LibraryModuleNames::kNeverthrowPath) {
    auto message = ErrorTextExportSource::neverthrow();

    return err(ModuleDeclarerExportSourceError{
        .code = ModuleDeclarerExportSourceErrorCode::NEVERTHROW,
        .message = message,
    });
  }
  if (source != nullptr && BuiltinModulePaths::isBuiltinModule(source->path)) {
    auto message = ErrorTextExportSource::builtin(source->path);

    return err(ModuleDeclarerExportSourceError{
        .code = ModuleDeclarerExportSourceErrorCode::BUILTIN_MODULE,
        .message = message,
    });
  }

  return ok(source);
}

ModuleDeclarer::ResultLocalIsKnown
ModuleDeclarer::localExport(const Module *module, const ModuleName &name, bool isFound) const {
  if (isFound) {
    return ok(&name);
  }
  if (this->isFromBuiltinModule(module, name.name)) {
    auto message = ErrorTextLocalExport::builtin(name.name);

    return err(ModuleDeclarerLocalExportError{
        .code = ModuleDeclarerLocalExportErrorCode::FROM_BUILTIN_MODULE,
        .message = message,
    });
  }
  if (!this->isImportedByName(module, name.name)) {
    auto message = ErrorTextLocalExport::notFound(name.name);

    return err(ModuleDeclarerLocalExportError{
        .code = ModuleDeclarerLocalExportErrorCode::NOT_FOUND,
        .message = message,
    });
  }

  return err(ModuleDeclarerLocalExportError{
      .code = ModuleDeclarerLocalExportErrorCode::ALREADY_REPORTED,
      .message = "",
  });
}

bool ModuleDeclarer::isNamedIn(const ImportDecl &importDecl, llvm::StringRef alias) const {
  for (const ModuleName &imported : importDecl.names) {
    if (imported.as == alias) {
      return true;
    }
  }

  return false;
}

bool ModuleDeclarer::isImportedByName(const Module *module, llvm::StringRef alias) const {
  for (const ImportDecl &importDecl : module->file.imports) {
    if (this->isNamedIn(importDecl, alias)) {
      return true;
    }
  }

  return false;
}

bool ModuleDeclarer::isFromBuiltinModule(const Module *module, llvm::StringRef alias) const {
  for (const ImportDecl &importDecl : module->file.imports) {
    bool isBringing = importDecl.whole == alias || this->isNamedIn(importDecl, alias);
    if (isBringing && BuiltinModulePaths::isBuiltinModule(importDecl.path)) {
      return true;
    }
  }

  return false;
}

ModuleDeclarer::ResultExportUnique ModuleDeclarer::freeExport(const ModuleState &here,
                                                              const ModuleName &name) const {
  if (here.exports.contains(name.as)) {
    auto message = ErrorTextFreeExport::duplicate(name.as);

    return err(ModuleDeclarerFreeExportError{
        .code = ModuleDeclarerFreeExportErrorCode::DUPLICATE_EXPORT,
        .message = message,
    });
  }

  return ok(&name);
}

void ModuleDeclarer::exportName(Module *module, const NamedExportStep &step) {
  auto &here = this->state.getModuleStates().getModuleState(module);
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
    this->state.getDiagnostics().error(name.asLoc, unique.error().message);
    this->state.getDiagnostics().note(exportedAt, previous);
    return;
  }
  auto exported = ModuleExport{.target = *target, .loc = name.asLoc, .all = nullptr};
  here.exports.try_emplace(name.as, exported);
}

std::optional<ModuleBinding> ModuleDeclarer::reexportedTarget(const ExportSource &source,
                                                              const ModuleName &name) {
  const auto &theirs = this->readyExports(source.from);
  auto found = this->exportNamed(theirs, source.from, source.path, name);
  if (found.isErr()) {
    this->state.getDiagnostics().error(name.loc, found.error().message);

    return std::nullopt;
  }

  return found.value();
}

std::optional<ModuleBinding> ModuleDeclarer::localTarget(Module *module, const ModuleName &name) {
  auto target = this->readyLocal(module, name.name);
  auto known = this->localExport(module, name, target.has_value());
  if (known.isOk()) {
    return target;
  }
  if (known.error().code != ModuleDeclarerLocalExportErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(name.loc, known.error().message);
  }

  return std::nullopt;
}

ModuleDeclarer::ResultStarIsUnique ModuleDeclarer::starExport(llvm::StringRef name,
                                                              const ModuleExport *ours,
                                                              const ExportAll *all,
                                                              const ModuleBinding &target) const {
  bool isClash = ours != nullptr && ours->all != nullptr && !(ours->target == target);
  if (isClash) {
    auto message = ErrorTextStarExport::twice(name, ours->all->source.path, all->source.path);

    return err(ModuleDeclarerStarExportError{
        .code = ModuleDeclarerStarExportErrorCode::EXPORTED_BY_TWO,
        .message = message,
    });
  }

  return ok(all);
}

const llvm::StringMap<ModuleExport> &ModuleDeclarer::exportsOf(Module *module) {
  auto &asked = this->state.getModuleStates().getModuleState(module);
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

void ModuleDeclarer::startExports(std::vector<ExportsFrame> &frames, Module *module) {
  this->state.getModuleStates().getModuleState(module).exportsBusy = true;
  auto steps = this->exportSteps(module);
  auto frame = ExportsFrame{.module = module, .steps = std::move(steps), .nextStep = 0};
  frames.push_back(std::move(frame));
}

std::vector<ExportStep> ModuleDeclarer::exportSteps(Module *module) const {
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

void ModuleDeclarer::addNameSteps(const ExportDecl &exportDecl,
                                  std::vector<ExportStep> &steps) const {
  for (const ModuleName &name : exportDecl.names()) {
    steps.push_back(NamedExportStep{.source = exportDecl.source(), .name = &name});
  }
}

Module *ModuleDeclarer::unreadyModule(const ExportsFrame &frame) {
  if (frame.nextStep == frame.steps.size()) {
    return nullptr;
  }
  auto *needed = this->stepNeeds(frame.module, frame.steps[frame.nextStep]);
  if (needed == nullptr) {
    return nullptr;
  }
  const auto &neededState = this->state.getModuleStates().getModuleState(needed);
  bool isWaiting = !neededState.exportsDone && !neededState.exportsBusy;

  return isWaiting ? needed : nullptr;
}

Module *ModuleDeclarer::stepNeeds(Module *module, const ExportStep &step) {
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

Module *ModuleDeclarer::namedExportNeeds(Module *module, const NamedExportStep &step) {
  if (step.source != nullptr) {
    return step.source->from;
  }
  const auto &here = this->state.getModuleStates().getModuleState(module);
  if (here.declared.contains(step.name->name)) {
    return nullptr;
  }
  auto imported = this->findImport(module, step.name->name);

  return imported.has_value() ? imported->from : nullptr;
}

void ModuleDeclarer::runExportStep(std::vector<ExportsFrame> &frames) {
  ExportsFrame &frame = frames.back();
  if (frame.nextStep == frame.steps.size()) {
    return this->finishExports(frames);
  }
  const ExportStep &step = frame.steps[frame.nextStep];
  frame.nextStep += 1;

  return this->applyExportStep(frame.module, step);
}

void ModuleDeclarer::applyExportStep(Module *module, const ExportStep &step) {
  switch (step.kind()) {
    case ExportStepKind::BAD_SOURCE:
      return this->reportBadSource(*step.as<BadSourceStep>());
    case ExportStepKind::NAMED:
      return this->exportName(module, *step.as<NamedExportStep>());
    case ExportStepKind::STAR:
      return this->mergeStarExport(module, *step.as<StarExportStep>());
  }
}

void ModuleDeclarer::reportBadSource(const BadSourceStep &step) {
  this->state.getDiagnostics().error(step.loc, step.message);
}

void ModuleDeclarer::mergeStarExport(Module *module, const StarExportStep &star) {
  auto &here = this->state.getModuleStates().getModuleState(module);
  const ExportDecl &exportDecl = *star.exportDecl;
  const ExportAll *all = exportDecl.form.as<ExportAll>();
  for (const auto &entry : this->readyExports(star.from)) {
    llvm::StringRef name = entry.getKey();
    const ModuleBinding &target = entry.getValue().target;
    const ModuleExport *ours = this->exportedAs(here, name);
    auto unique = this->starExport(name, ours, all, target);
    if (unique.isErr()) {
      this->state.getDiagnostics().error(exportDecl.loc, unique.error().message);
    }
    auto exported = ModuleExport{.target = target, .loc = exportDecl.loc, .all = all};
    here.exports.try_emplace(name, exported);
  }
}

const ModuleExport *ModuleDeclarer::exportedAs(const ModuleState &here,
                                               llvm::StringRef name) const {
  auto existing = here.exports.find(name);
  if (existing == here.exports.end()) {
    return nullptr;
  }

  return &existing->second;
}

void ModuleDeclarer::finishExports(std::vector<ExportsFrame> &frames) {
  auto &done = this->state.getModuleStates().getModuleState(frames.back().module);
  done.exportsBusy = false;
  done.exportsDone = true;
  frames.pop_back();
}

ModuleDeclarer::ResultExportTarget ModuleDeclarer::findExport(Module *from, llvm::StringRef path,
                                                              const ModuleName &name) {
  const auto &theirs = this->exportsOf(from);

  return this->exportNamed(theirs, from, path, name);
}

const llvm::StringMap<ModuleExport> &ModuleDeclarer::readyExports(Module *module) {
  return this->state.getModuleStates().getModuleState(module).exports;
}

ModuleDeclarer::ResultExportTarget
ModuleDeclarer::exportNamed(const llvm::StringMap<ModuleExport> &theirs, Module *from,
                            llvm::StringRef path, const ModuleName &name) {
  auto found = theirs.find(name.name);
  bool isMissing = found == theirs.end();
  if (isMissing && from->builtin) {
    auto hint = ErrorTextEventsExport::hint(name.name);
    auto message = ErrorTextFindExport::library(path, name.name, hint);

    return err(ModuleDeclarerFindExportError{
        .code = ModuleDeclarerFindExportErrorCode::NOT_EXPORTED_BY_LIBRARY,
        .message = message,
    });
  }
  const auto &theirState = this->state.getModuleStates().getModuleState(from);
  if (isMissing && theirState.declared.contains(name.name)) {
    auto message = ErrorTextFindExport::unmarked(path, name.name, from->name);

    return err(ModuleDeclarerFindExportError{
        .code = ModuleDeclarerFindExportErrorCode::DECLARED_WITHOUT_EXPORT,
        .message = message,
    });
  }
  if (isMissing) {
    auto message = ErrorTextFindExport::missing(path, name.name);

    return err(ModuleDeclarerFindExportError{
        .code = ModuleDeclarerFindExportErrorCode::NOT_EXPORTED,
        .message = message,
    });
  }

  return ok(found->second.target);
}

std::optional<ModuleBinding> ModuleDeclarer::readyLocal(Module *module, llvm::StringRef name) {
  if (this->state.getModuleStates().getModuleState(module).declared.contains(name)) {
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

std::optional<ImportedName> ModuleDeclarer::findImport(const Module *module,
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

}
