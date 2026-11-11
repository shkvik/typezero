#include "compiler/semantic/declare/import_binder/import_binder.h"
#include "compiler/semantic/declare/module_declarer/module_declarer.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/semantic/library/result/result_types/result_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

ImportBinder::ImportBinder(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->moduleDeclarer = &all.moduleDeclarer;
  this->builtinModuleChecker = &all.builtinModuleChecker;
}

ImportBinder::ResultWholeImportOk ImportBinder::wholeImport(const ImportDecl &decl) {
  bool isWhole = !decl.whole.empty() && decl.from && decl.from->builtin;
  if (isWhole && decl.wholeIsDefault) {
    std::string message = ErrorTextWholeImport::byDefault(decl.path);

    return err(ImportBinderWholeImportError{
        .code = ImportBinderWholeImportErrorCode::DEFAULT_IMPORT,
        .message = message,
    });
  }
  if (isWhole) {
    std::string message = ErrorTextWholeImport::star(decl.path);

    return err(ImportBinderWholeImportError{
        .code = ImportBinderWholeImportErrorCode::STAR_IMPORT,
        .message = message,
    });
  }

  return ok(&decl);
}

ImportBinder::ResultAliasIsUnused
ImportBinder::freeName(const ModuleState &here, const ImportDecl &decl, const ModuleName &name) {
  if (here.declared.count(name.as)) {
    std::string message = ErrorTextFreeName::conflicts(name.as, name.name, decl.path);

    return err(ImportBinderFreeNameError{
        .code = ImportBinderFreeNameErrorCode::DECLARED_IN_FILE,
        .message = message,
    });
  }
  if (here.nodeNames.count(name.as)) {
    std::string message = ErrorTextFreeName::duplicate(name.as);

    return err(ImportBinderFreeNameError{
        .code = ImportBinderFreeNameErrorCode::IMPORTED_FROM_NODE,
        .message = message,
    });
  }
  if (here.imports.count(name.as)) {
    std::string message = ErrorTextFreeName::duplicate(name.as);

    return err(ImportBinderFreeNameError{
        .code = ImportBinderFreeNameErrorCode::IMPORTED_TWICE,
        .message = message,
    });
  }

  return ok(&name);
}

void ImportBinder::reportTaken(const ModuleState &here, const ModuleName &name,
                               const ImportBinderFreeNameError &error) {
  this->state.getDiagnostics().error(name.asLoc, error.message);
  switch (error.code) {
    case ImportBinderFreeNameErrorCode::DECLARED_IN_FILE: {
      llvm::SMLoc declaredAt = here.declared.find(name.as)->second;
      std::string declared = ErrorTextFreeName::declared(name.as);
      this->state.getDiagnostics().note(declaredAt, declared);
      break;
    }
    case ImportBinderFreeNameErrorCode::IMPORTED_FROM_NODE:
      break;
    case ImportBinderFreeNameErrorCode::IMPORTED_TWICE: {
      llvm::SMLoc importedAt = here.imports.find(name.as)->second.loc;
      std::string previous = ErrorTextFreeName::previous();
      this->state.getDiagnostics().note(importedAt, previous);
      break;
    }
  }
}

void ImportBinder::importName(ModuleState &here, const ImportDecl &decl, const ModuleName &name) {
  if (!decl.from) {
    here.badImports.insert(name.as);
    return;
  }
  ModuleDeclarer::ResultExportTarget target =
      this->moduleDeclarer->findExport(decl.from, decl.path, name);
  if (target.isErr()) {
    this->state.getDiagnostics().error(name.loc, target.error().message);
    here.badImports.insert(name.as);
    return;
  }
  ResultAliasIsUnused unused = ImportBinder::freeName(here, decl, name);
  if (unused.isErr()) {
    this->reportTaken(here, name, unused.error());
    return;
  }
  here.imports.try_emplace(name.as, ModuleImport{target.value(), name.asLoc});
}

void ImportBinder::resolveImports(Module *module) {
  ModuleState &here = this->state.getModuleStates().getModuleState(module);
  for (const ImportDecl &decl : module->file.imports) {
    ResultTypes::ResultImportSource source = ResultTypes::importSource(decl);
    if (source.isErr()) {
      this->state.getDiagnostics().error(decl.pathLoc, source.error().message);
      for (const ModuleName &name : decl.names) {
        here.badImports.insert(name.as);
      }
      continue;
    }
    if (BuiltinModulePaths::isBuiltinModule(decl.path) && !decl.from) {
      this->builtinModuleChecker->importBuiltinModule(module, decl);
      continue;
    }
    ResultWholeImportOk whole = ImportBinder::wholeImport(decl);
    if (whole.isErr()) {
      this->state.getDiagnostics().error(decl.wholeLoc, whole.error().message);
      here.badImports.insert(decl.whole);
    }
    for (const ModuleName &name : decl.names) {
      this->importName(here, decl, name);
    }
  }
}

void ImportBinder::bindImports() {
  ModuleState &here = this->state.getModuleStates().getModuleState(
      this->state.getModuleStates().getCurrentModule());
  for (const auto &entry : here.nodeNames) {
    const NodeName &n = entry.getValue();
    ClassDecl *cls = nullptr;
    if (n.module == NodeModule::Fs && n.name == "Stats") {
      cls = this->state.getFileSystemStore().getStatsClass();
    } else if (n.module == NodeModule::Fs && n.name == "Dirent") {
      cls = this->state.getFileSystemStore().getDirentClass();
    }
    if (!cls) {
      continue;
    }
    this->state.getScopeStack().addTopType(entry.getKey(), cls);
    this->state.getScopeStack().addTopValue(entry.getKey(), ScopeEntry{ClassEntry{.cls = cls}});
  }
  for (const auto &entry : here.imports) {
    llvm::StringRef alias = entry.getKey();
    const ModuleBinding &target = entry.getValue().target;
    Module *source = target.module;
    bool isOwn = source == this->state.getModuleStates().getCurrentModule();
    ScopeStack &scopes = this->state.getScopeStack();
    if (!scopes.hasTopType(alias)) {
      Stmt *type = this->exportedType(source, target.name);
      if (type) {
        scopes.addTopType(alias, type);
      }
    }
    if (scopes.hasTopValue(alias)) {
      continue;
    }
    std::optional<ScopeEntry> found = this->exportedValue(source, target.name);
    if (!found) {
      continue;
    }
    ScopeEntry value = *found;
    if (!value.found()) {
      continue;
    }
    scopes.addTopValue(alias, value);
    if (value.var() && !isOwn) {
      here.importedVars.insert(value.var());
    }
  }
}

void ImportBinder::enterModule(Module *module) {
  this->moduleDeclarer->useModule(module);
  this->bindImports();
}

void ImportBinder::enterHomeModule(const FunctionDecl *fn) {
  Module *home = this->state.getModuleStates().findDeclModule(fn);
  if (home != nullptr) {
    this->enterModule(home);
  }
}

void ImportBinder::bindAllImports(const std::vector<Module *> &modules) {
  for (Module *module : modules) {
    this->enterModule(module);
  }
}

Stmt *ImportBinder::exportedType(Module *source, llvm::StringRef name) {
  if (source == this->state.getModuleStates().getCurrentModule()) {
    return this->state.getScopeStack().findTopType(name);
  }

  return this->state.getModuleStates().getModuleState(source).types.lookup(name);
}

std::optional<ScopeEntry> ImportBinder::exportedValue(Module *source, llvm::StringRef name) {
  if (source == this->state.getModuleStates().getCurrentModule()) {
    return this->state.getScopeStack().findTopValue(name);
  }
  const llvm::StringMap<ScopeEntry> &values =
      this->state.getModuleStates().getModuleState(source).values;
  auto found = values.find(name);
  if (found == values.end()) {
    return std::nullopt;
  }

  return found->second;
}

void ImportBinder::noteImportsInFunctions(SourceFile &program) {
  for (Module *module : program.modules) {
    for (const auto &entry : this->state.getModuleStates().getModuleState(module).imports) {
      if (this->state.getScopeStack().isUsedInFunction(entry.getKey())) {
        this->state.getScopeStack().markUsedInFunction(entry.getValue().target.name);
      }
    }
  }
}

FunctionDecl *ImportBinder::cycleCallee(FunctionDecl *fn, llvm::StringRef name) {
  Module *home = this->state.getModuleStates().findDeclModule(fn);
  if (!home) {
    return nullptr;
  }
  std::optional<ScopeEntry> found = this->exportedValue(home, name);
  if (!found || !found->func()) {
    return nullptr;
  }

  return found->func()->decl;
}

}
