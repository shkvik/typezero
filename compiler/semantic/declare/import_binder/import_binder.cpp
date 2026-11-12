#include "compiler/semantic/declare/import_binder/import_binder.h"
#include "compiler/program/library/fs/fs_function_table/fs_function_table.h"
#include "compiler/semantic/declare/module_declarer/module_declarer.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/semantic/library/result/result_types/result_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

ImportBinder::ImportBinder(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->resultTypes = &all.resultTypes;
  this->moduleDeclarer = &all.moduleDeclarer;
  this->builtinModuleChecker = &all.builtinModuleChecker;
}

ImportBinder::ResultWholeImportOk ImportBinder::wholeImport(const ImportDecl &importDecl) const {
  bool isWhole =
      !importDecl.whole.empty() && importDecl.from != nullptr && importDecl.from->builtin;
  if (isWhole && importDecl.wholeIsDefault) {
    auto message = ErrorTextWholeImport::byDefault(importDecl.path);

    return err(ImportBinderWholeImportError{
        .code = ImportBinderWholeImportErrorCode::DEFAULT_IMPORT,
        .message = message,
    });
  }
  if (isWhole) {
    auto message = ErrorTextWholeImport::star(importDecl.path);

    return err(ImportBinderWholeImportError{
        .code = ImportBinderWholeImportErrorCode::STAR_IMPORT,
        .message = message,
    });
  }

  return ok(&importDecl);
}

ImportBinder::ResultAliasIsUnused ImportBinder::freeName(const ModuleState &here,
                                                         const ImportDecl &importDecl,
                                                         const ModuleName &name) const {
  if (here.declared.contains(name.as)) {
    auto message = ErrorTextFreeName::conflicts(name.as, name.name, importDecl.path);

    return err(ImportBinderFreeNameError{
        .code = ImportBinderFreeNameErrorCode::DECLARED_IN_FILE,
        .message = message,
    });
  }
  if (here.nodeNames.contains(name.as)) {
    auto message = ErrorTextFreeName::duplicate(name.as);

    return err(ImportBinderFreeNameError{
        .code = ImportBinderFreeNameErrorCode::IMPORTED_FROM_NODE,
        .message = message,
    });
  }
  if (here.imports.contains(name.as)) {
    auto message = ErrorTextFreeName::duplicate(name.as);

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
      auto declared = ErrorTextFreeName::declared(name.as);
      this->state.getDiagnostics().note(declaredAt, declared);
      break;
    }
    case ImportBinderFreeNameErrorCode::IMPORTED_FROM_NODE:
      break;
    case ImportBinderFreeNameErrorCode::IMPORTED_TWICE: {
      llvm::SMLoc importedAt = here.imports.find(name.as)->second.loc;
      auto previous = ErrorTextFreeName::previous();
      this->state.getDiagnostics().note(importedAt, previous);
      break;
    }
  }
}

void ImportBinder::importName(ModuleState &here, const ImportDecl &importDecl,
                              const ModuleName &name) {
  if (importDecl.from == nullptr) {
    here.badImports.insert(name.as);
    return;
  }
  auto target = this->moduleDeclarer->findExport(importDecl.from, importDecl.path, name);
  if (target.isErr()) {
    this->state.getDiagnostics().error(name.loc, target.error().message);
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

void ImportBinder::resolveImports(Module *module) {
  for (const ImportDecl &importDecl : module->file.imports) {
    this->resolveImport(module, importDecl);
  }
}

void ImportBinder::resolveImport(Module *module, const ImportDecl &importDecl) {
  auto &here = this->state.getModuleStates().getModuleState(module);
  auto source = this->resultTypes->importSource(importDecl);
  if (source.isErr()) {
    this->state.getDiagnostics().error(importDecl.pathLoc, source.error().message);
    for (const ModuleName &name : importDecl.names) {
      here.badImports.insert(name.as);
    }
    return;
  }
  if (BuiltinModulePaths::isBuiltinModule(importDecl.path) && importDecl.from == nullptr) {
    this->builtinModuleChecker->importBuiltinModule(module, importDecl);
    return;
  }
  auto whole = this->wholeImport(importDecl);
  if (whole.isErr()) {
    this->state.getDiagnostics().error(importDecl.wholeLoc, whole.error().message);
    here.badImports.insert(importDecl.whole);
  }
  for (const ModuleName &name : importDecl.names) {
    this->importName(here, importDecl, name);
  }
}

void ImportBinder::bindImports() {
  auto *current = this->state.getModuleStates().getCurrentModule();
  auto &here = this->state.getModuleStates().getModuleState(current);
  for (const auto &entry : here.nodeNames) {
    auto *classDecl = this->nodeClass(entry.getValue());
    if (classDecl != nullptr) {
      auto value = ScopeEntry{ClassEntry{.cls = classDecl}};
      this->state.getScopeStack().addTopType(entry.getKey(), classDecl);
      this->state.getScopeStack().addTopValue(entry.getKey(), value);
    }
  }
  for (const auto &entry : here.imports) {
    this->bindImportedType(entry.getKey(), entry.getValue().target);
    this->bindImportedValue(here, entry.getKey(), entry.getValue().target);
  }
}

ClassDecl *ImportBinder::nodeClass(const NodeName &nodeName) const {
  if (nodeName.module != NodeModule::Fs) {
    return nullptr;
  }
  auto found = FsFunctionTable::find(nodeName.name);
  if (found == FsFunction::STATS) {
    return this->state.getFileSystemStore().getStatsClass();
  }
  if (found == FsFunction::DIRENT) {
    return this->state.getFileSystemStore().getDirentClass();
  }

  return nullptr;
}

void ImportBinder::bindImportedType(llvm::StringRef alias, const ModuleBinding &target) {
  auto &scopes = this->state.getScopeStack();
  if (scopes.hasTopType(alias)) {
    return;
  }
  auto *type = this->exportedType(target.module, target.name);
  if (type != nullptr) {
    scopes.addTopType(alias, type);
  }
}

void ImportBinder::bindImportedValue(ModuleState &here, llvm::StringRef alias,
                                     const ModuleBinding &target) {
  auto &scopes = this->state.getScopeStack();
  if (scopes.hasTopValue(alias)) {
    return;
  }
  auto found = this->exportedValue(target.module, target.name);
  if (!found.has_value() || !found->isFound()) {
    return;
  }
  scopes.addTopValue(alias, *found);
  bool isOwn = target.module == this->state.getModuleStates().getCurrentModule();
  if (found->var() != nullptr && !isOwn) {
    here.importedVars.insert(found->var());
  }
}

void ImportBinder::enterModule(Module *module) {
  this->moduleDeclarer->useModule(module);
  this->bindImports();
}

void ImportBinder::enterHomeModule(const FunctionDecl *functionDecl) {
  auto *home = this->state.getModuleStates().findDeclModule(functionDecl);
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

FunctionDecl *ImportBinder::cycleCallee(FunctionDecl *functionDecl, llvm::StringRef name) {
  auto *home = this->state.getModuleStates().findDeclModule(functionDecl);
  if (home == nullptr) {
    return nullptr;
  }
  auto found = this->exportedValue(home, name);
  if (!found.has_value() || found->func() == nullptr) {
    return nullptr;
  }

  return found->func()->decl;
}

}
