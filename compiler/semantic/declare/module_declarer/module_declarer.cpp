#include "compiler/semantic/declare/module_declarer/module_declarer.h"
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
  if (module == this->state.getModuleStates().getCurrentModule()) {
    return;
  }
  assert(this->state.getScopeStack().isTopLevel() &&
         this->state.getScopeStack().isTopLevelTypes() && "a file changes only at the top level");
  if (this->state.getModuleStates().getCurrentModule()) {
    ModuleState &old = this->state.getModuleStates().getModuleState(
        this->state.getModuleStates().getCurrentModule());
    old.values = this->state.getScopeStack().getTopValues();
    old.types = this->state.getScopeStack().getTopTypes();
  }
  this->state.getModuleStates().rememberCurrentModule(module);
  this->state.getScopeStack().replaceTopValues(
      this->state.getModuleStates().getModuleState(module).values);
  this->state.getScopeStack().replaceTopTypes(
      this->state.getModuleStates().getModuleState(module).types);
}

void ModuleDeclarer::declareModules(SourceFile &program) {
  this->state.getModuleStates().prepareModules(int(program.modules.size()));
  for (Module *module : program.modules) {
    ModuleState &here = this->state.getModuleStates().getModuleState(module);
    for (Stmt *item : module->file.items) {
      ModuleDeclarer::addDeclaredItem(here, item);
    }
  }
  std::vector<std::vector<Stmt *>> types = this->declareModuleTypes(program);
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
  std::vector<FunctionDecl *> functions;
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

void ModuleDeclarer::addDeclaredItem(ModuleState &here, const Stmt *item) {
  auto *decl = dyn_cast<VarDeclStmt>(item);
  if (decl != nullptr) {
    for (const VarDeclarator &d : decl->decls) {
      ModuleDeclarer::addDeclared(here, d.name, d.loc);
      here.topLevelNames.insert(d.name);
    }
    return;
  }
  std::optional<llvm::StringRef> name = ModuleDeclarer::itemName(item);
  if (name.has_value()) {
    ModuleDeclarer::addDeclared(here, *name, item->loc());
  }
}

void ModuleDeclarer::addDeclared(ModuleState &here, llvm::StringRef name, llvm::SMLoc loc) {
  if (!name.empty()) {
    here.declared.try_emplace(name, loc);
  }
}

std::optional<llvm::StringRef> ModuleDeclarer::itemName(const Stmt *item) {
  auto *fn = dyn_cast<FunctionDecl>(item);
  if (fn != nullptr) {
    return llvm::StringRef(fn->name);
  }
  auto *alias = dyn_cast<TypeAliasDecl>(item);
  if (alias != nullptr) {
    return llvm::StringRef(alias->name);
  }
  auto *e = dyn_cast<EnumDecl>(item);
  if (e != nullptr) {
    return llvm::StringRef(e->name);
  }
  auto *cls = dyn_cast<ClassDecl>(item);
  if (cls != nullptr) {
    return llvm::StringRef(cls->name);
  }

  return std::nullopt;
}

std::vector<std::vector<Stmt *>> ModuleDeclarer::declareModuleTypes(SourceFile &program) {
  std::vector<std::vector<Stmt *>> types(program.modules.size());
  for (Module *module : program.modules) {
    this->useModule(module);
    types[module->order] = this->typeDeclarer->declareTypes(module->file.items, false);
    for (Stmt *type : types[module->order]) {
      this->state.getModuleStates().rememberDeclModule(type, module);
    }
  }

  return types;
}

void ModuleDeclarer::declareModuleFunctions(Module *module,
                                            std::vector<FunctionDecl *> &functions) {
  for (Stmt *item : module->file.items) {
    auto *fn = dyn_cast<FunctionDecl>(item);
    if (fn != nullptr) {
      this->functionChecker->declareFunction(fn);
      this->state.getModuleStates().rememberDeclModule(fn, module);
      functions.push_back(fn);
    }
    auto *cls = dyn_cast<ClassDecl>(item);
    if (cls == nullptr || cls->type == nullptr || cls->baseExpr) {
      continue;
    }
    this->classDeclarer->declareMethods(cls);
    for (FunctionDecl *method : ModuleDeclarer::classFunctions(cls)) {
      this->state.getModuleStates().rememberDeclModule(method, module);
      functions.push_back(method);
    }
  }
}

std::vector<FunctionDecl *> ModuleDeclarer::classFunctions(const ClassDecl *cls) {
  std::vector<FunctionDecl *> methods = cls->methods;
  methods.insert(methods.end(), cls->constructors.begin(), cls->constructors.end());
  if (cls->init != nullptr) {
    methods.push_back(cls->init);
  }
  for (const ClassField &f : cls->fields) {
    if (f.method()) {
      methods.push_back(f.method());
    }
  }

  return methods;
}

void ModuleDeclarer::fillUnfilledInstances() {
  GenericStore &generics = this->state.getGenericStore();
  for (ClassDecl *instance : generics.takeUnfilledClasses()) {
    generics.enterFilling(instance);
    this->genericClassInstantiator->fillInstance(instance);
    generics.leaveFilling();
  }
}

ModuleDeclarer::ResultSourceUsable ModuleDeclarer::exportSource(const ExportSource *source) {
  if (source && source->path == "neverthrow") {
    std::string message = ErrorTextExportSource::neverthrow();

    return err(ModuleDeclarerExportSourceError{
        .code = ModuleDeclarerExportSourceErrorCode::NEVERTHROW,
        .message = message,
    });
  }
  if (source && BuiltinModulePaths::isBuiltinModule(source->path)) {
    std::string message = ErrorTextExportSource::builtin(source->path);

    return err(ModuleDeclarerExportSourceError{
        .code = ModuleDeclarerExportSourceErrorCode::BUILTIN_MODULE,
        .message = message,
    });
  }

  return ok(source);
}

ModuleDeclarer::ResultLocalIsKnown ModuleDeclarer::localExport(const Module *module,
                                                               const ModuleName &name, bool found) {
  auto sameAlias = [&](const ModuleName &n) { return n.as == name.name; };
  auto fromBuiltin = [&](const ImportDecl &i) {
    return BuiltinModulePaths::isBuiltinModule(i.path) &&
           (i.whole == name.name || llvm::any_of(i.names, sameAlias));
  };
  auto importsName = [&](const ImportDecl &i) { return llvm::any_of(i.names, sameAlias); };
  bool builtin = llvm::any_of(module->file.imports, fromBuiltin);
  if (!found && builtin) {
    std::string message = ErrorTextLocalExport::builtin(name.name);

    return err(ModuleDeclarerLocalExportError{
        .code = ModuleDeclarerLocalExportErrorCode::FROM_BUILTIN_MODULE,
        .message = message,
    });
  }
  bool imported = llvm::any_of(module->file.imports, importsName);
  if (!found && !imported) {
    std::string message = ErrorTextLocalExport::notFound(name.name);

    return err(ModuleDeclarerLocalExportError{
        .code = ModuleDeclarerLocalExportErrorCode::NOT_FOUND,
        .message = message,
    });
  }
  if (!found) {
    return err(ModuleDeclarerLocalExportError{
        .code = ModuleDeclarerLocalExportErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }

  return ok(&name);
}

ModuleDeclarer::ResultExportUnique ModuleDeclarer::freeExport(const ModuleState &here,
                                                              const ModuleName &name) {
  if (here.exports.count(name.as)) {
    std::string message = ErrorTextFreeExport::duplicate(name.as);

    return err(ModuleDeclarerFreeExportError{
        .code = ModuleDeclarerFreeExportErrorCode::DUPLICATE_EXPORT,
        .message = message,
    });
  }

  return ok(&name);
}

void ModuleDeclarer::exportName(Module *module, const NamedExportStep &step) {
  ModuleState &here = this->state.getModuleStates().getModuleState(module);
  const ExportSource *source = step.source;
  const ModuleName &name = *step.name;
  if (source && !source->from) {
    return;
  }
  std::optional<ModuleBinding> target;
  if (source) {
    const llvm::StringMap<ModuleExport> &theirs = this->readyExports(source->from);
    ResultExportTarget found = this->exportNamed(theirs, source->from, source->path, name);
    if (found.isErr()) {
      this->state.getDiagnostics().error(name.loc, found.error().message);
      return;
    }
    target = found.value();
  } else {
    target = this->readyLocal(module, name.name);
    ResultLocalIsKnown known = ModuleDeclarer::localExport(module, name, target.has_value());
    bool reported =
        known.isErr() && known.error().code == ModuleDeclarerLocalExportErrorCode::ALREADY_REPORTED;
    if (known.isErr() && !reported) {
      this->state.getDiagnostics().error(name.loc, known.error().message);
    }
    if (known.isErr()) {
      return;
    }
  }
  ResultExportUnique unique = ModuleDeclarer::freeExport(here, name);
  if (unique.isErr()) {
    llvm::SMLoc exportedAt = here.exports.find(name.as)->second.loc;
    std::string previous = ErrorTextFreeExport::previous();
    this->state.getDiagnostics().error(name.asLoc, unique.error().message);
    this->state.getDiagnostics().note(exportedAt, previous);
    return;
  }
  here.exports.try_emplace(name.as, ModuleExport{*target, name.asLoc, nullptr});
}

ModuleDeclarer::ResultStarIsUnique ModuleDeclarer::starExport(llvm::StringRef name,
                                                              const ModuleExport *ours,
                                                              const ExportAll *all,
                                                              const ModuleBinding &target) {
  bool clash = ours && ours->all && !(ours->target == target);
  if (clash) {
    std::string message =
        ErrorTextStarExport::twice(name, ours->all->source.path, all->source.path);

    return err(ModuleDeclarerStarExportError{
        .code = ModuleDeclarerStarExportErrorCode::EXPORTED_BY_TWO,
        .message = message,
    });
  }

  return ok(all);
}

const llvm::StringMap<ModuleExport> &ModuleDeclarer::exportsOf(Module *module) {
  ModuleState &asked = this->state.getModuleStates().getModuleState(module);
  if (asked.exportsDone || asked.exportsBusy) {
    return asked.exports;
  }
  std::vector<ExportsFrame> frames;
  this->startExports(frames, module);
  while (!frames.empty()) {
    Module *needed = this->unreadyModule(frames.back());
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
  std::vector<ExportStep> steps = ModuleDeclarer::exportSteps(module);
  frames.push_back(ExportsFrame{module, std::move(steps), 0});
}

std::vector<ExportStep> ModuleDeclarer::exportSteps(Module *module) {
  std::vector<ExportStep> steps;
  for (const ExportDecl &decl : module->file.exports) {
    ResultSourceUsable usable = ModuleDeclarer::exportSource(decl.source());
    if (usable.isOk()) {
      ModuleDeclarer::addNameSteps(decl, steps);
    } else {
      steps.push_back(BadSourceStep{decl.source()->pathLoc, usable.error().message});
    }
  }
  for (const ExportDecl &decl : module->file.exports) {
    const ExportAll *all = decl.form.as<ExportAll>();
    bool fromOther = all != nullptr && all->source.from != nullptr && all->source.from != module;
    if (fromOther) {
      steps.push_back(StarExportStep{&decl, all->source.from});
    }
  }

  return steps;
}

void ModuleDeclarer::addNameSteps(const ExportDecl &decl, std::vector<ExportStep> &steps) {
  for (const ModuleName &name : decl.names()) {
    steps.push_back(NamedExportStep{decl.source(), &name});
  }
}

Module *ModuleDeclarer::unreadyModule(const ExportsFrame &frame) {
  if (frame.nextStep == frame.steps.size()) {
    return nullptr;
  }
  Module *needed = this->stepNeeds(frame.module, frame.steps[frame.nextStep]);
  if (needed == nullptr) {
    return nullptr;
  }
  const ModuleState &neededState = this->state.getModuleStates().getModuleState(needed);
  bool waiting = !neededState.exportsDone && !neededState.exportsBusy;

  return waiting ? needed : nullptr;
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
  bool declared =
      this->state.getModuleStates().getModuleState(module).declared.count(step.name->name);
  if (declared) {
    return nullptr;
  }
  std::optional<ImportedName> imported = ModuleDeclarer::findImport(module, step.name->name);

  return imported.has_value() ? imported->from : nullptr;
}

void ModuleDeclarer::runExportStep(std::vector<ExportsFrame> &frames) {
  ExportsFrame &frame = frames.back();
  if (frame.nextStep == frame.steps.size()) {
    this->finishExports(frames);
  } else {
    const ExportStep &step = frame.steps[frame.nextStep];
    frame.nextStep += 1;
    this->applyExportStep(frame.module, step);
  }
}

void ModuleDeclarer::applyExportStep(Module *module, const ExportStep &step) {
  switch (step.kind()) {
    case ExportStepKind::BAD_SOURCE:
      this->reportBadSource(*step.as<BadSourceStep>());
      break;
    case ExportStepKind::NAMED:
      this->exportName(module, *step.as<NamedExportStep>());
      break;
    case ExportStepKind::STAR:
      this->mergeStarExport(module, *step.as<StarExportStep>());
      break;
  }
}

void ModuleDeclarer::reportBadSource(const BadSourceStep &step) {
  this->state.getDiagnostics().error(step.loc, step.message);
}

void ModuleDeclarer::mergeStarExport(Module *module, const StarExportStep &star) {
  ModuleState &here = this->state.getModuleStates().getModuleState(module);
  const ExportDecl &decl = *star.decl;
  const ExportAll *all = decl.form.as<ExportAll>();
  for (const auto &entry : this->readyExports(star.from)) {
    llvm::StringRef name = entry.getKey();
    const ModuleBinding &target = entry.getValue().target;
    auto existing = here.exports.find(name);
    const ModuleExport *ours = existing != here.exports.end() ? &existing->second : nullptr;
    ResultStarIsUnique unique = ModuleDeclarer::starExport(name, ours, all, target);
    if (unique.isErr()) {
      this->state.getDiagnostics().error(decl.loc, unique.error().message);
    }
    here.exports.try_emplace(name, ModuleExport{target, decl.loc, all});
  }
}

void ModuleDeclarer::finishExports(std::vector<ExportsFrame> &frames) {
  ModuleState &done = this->state.getModuleStates().getModuleState(frames.back().module);
  done.exportsBusy = false;
  done.exportsDone = true;
  frames.pop_back();
}

ModuleDeclarer::ResultExportTarget ModuleDeclarer::findExport(Module *from, llvm::StringRef path,
                                                              const ModuleName &name) {
  const llvm::StringMap<ModuleExport> &theirs = this->exportsOf(from);

  return this->exportNamed(theirs, from, path, name);
}

const llvm::StringMap<ModuleExport> &ModuleDeclarer::readyExports(Module *module) {
  return this->state.getModuleStates().getModuleState(module).exports;
}

ModuleDeclarer::ResultExportTarget
ModuleDeclarer::exportNamed(const llvm::StringMap<ModuleExport> &theirs, Module *from,
                            llvm::StringRef path, const ModuleName &name) {
  auto found = theirs.find(name.name);
  bool missing = found == theirs.end();
  if (missing && from->builtin) {
    std::string hint = ErrorTextEventsExport::hint(name.name);
    std::string message = ErrorTextFindExport::library(path, name.name, hint);

    return err(ModuleDeclarerFindExportError{
        .code = ModuleDeclarerFindExportErrorCode::NOT_EXPORTED_BY_LIBRARY,
        .message = message,
    });
  }
  bool declared =
      missing && this->state.getModuleStates().getModuleState(from).declared.count(name.name);
  if (declared) {
    std::string message = ErrorTextFindExport::unmarked(path, name.name, from->name);

    return err(ModuleDeclarerFindExportError{
        .code = ModuleDeclarerFindExportErrorCode::DECLARED_WITHOUT_EXPORT,
        .message = message,
    });
  }
  if (missing) {
    std::string message = ErrorTextFindExport::missing(path, name.name);

    return err(ModuleDeclarerFindExportError{
        .code = ModuleDeclarerFindExportErrorCode::NOT_EXPORTED,
        .message = message,
    });
  }

  return ok(found->second.target);
}

std::optional<ModuleBinding> ModuleDeclarer::readyLocal(Module *module, llvm::StringRef name) {
  if (this->state.getModuleStates().getModuleState(module).declared.count(name)) {
    return ModuleBinding{module, name};
  }
  std::optional<ImportedName> imported = ModuleDeclarer::findImport(module, name);
  if (!imported.has_value() || imported->from == nullptr) {
    return std::nullopt;
  }
  const llvm::StringMap<ModuleExport> &theirs = this->readyExports(imported->from);
  auto found = theirs.find(imported->name);
  if (found == theirs.end()) {
    return std::nullopt;
  }

  return found->second.target;
}

std::optional<ImportedName> ModuleDeclarer::findImport(const Module *module, llvm::StringRef name) {
  for (const ImportDecl &decl : module->file.imports) {
    for (const ModuleName &imported : decl.names) {
      if (imported.as == name) {
        return ImportedName{decl.from, imported.name};
      }
    }
  }

  return std::nullopt;
}

}
