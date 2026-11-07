#include "compiler/declare/import_binder/import_binder.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/result/result_types/result_types.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

ImportBinder::ImportBinder(Checker &owner)
    : checker(owner) {}

ImportBinder::ResultWholeImportOk ImportBinder::wholeImport(const ImportDecl &decl) {
  bool whole = !decl.whole.empty() && decl.from && decl.from->builtin;
  if (whole && decl.wholeIsDefault) {
    std::string message = ErrorTextWholeImport::byDefault(decl.path);

    return err(ImportBinderWholeImportError{
        .code = ImportBinderWholeImportErrorCode::DEFAULT_IMPORT,
        .message = message,
    });
  }
  if (whole) {
    std::string message = ErrorTextWholeImport::star(decl.path);

    return err(ImportBinderWholeImportError{
        .code = ImportBinderWholeImportErrorCode::STAR_IMPORT,
        .message = message,
    });
  }

  return ok(&decl);
}

ImportBinder::ResultAliasIsUnused ImportBinder::freeName(const Checker::ModuleState &here,
                                                         const ImportDecl &decl,
                                                         const ModuleName &name) {
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

void ImportBinder::reportTaken(const Checker::ModuleState &here, const ModuleName &name,
                               const ImportBinderFreeNameError &error) {
  this->checker.diags_.error(name.asLoc, error.message);
  switch (error.code) {
    case ImportBinderFreeNameErrorCode::DECLARED_IN_FILE: {
      llvm::SMLoc declaredAt = here.declared.find(name.as)->second;
      std::string declared = ErrorTextFreeName::declared(name.as);
      this->checker.diags_.note(declaredAt, declared);
      break;
    }
    case ImportBinderFreeNameErrorCode::IMPORTED_FROM_NODE:
      break;
    case ImportBinderFreeNameErrorCode::IMPORTED_TWICE: {
      llvm::SMLoc importedAt = here.imports.find(name.as)->second.loc;
      std::string previous = ErrorTextFreeName::previous();
      this->checker.diags_.note(importedAt, previous);
      break;
    }
  }
}

void ImportBinder::importName(Checker::ModuleState &here, const ImportDecl &decl,
                              const ModuleName &name) {
  if (!decl.from) {
    here.badImports.insert(name.as);
    return;
  }
  ModuleDeclarer::ResultExportTarget target =
      this->checker.moduleDeclarer().findExport(decl.from, decl.path, name);
  if (target.isErr()) {
    this->checker.diags_.error(name.loc, target.error().message);
    here.badImports.insert(name.as);
    return;
  }
  ResultAliasIsUnused unused = ImportBinder::freeName(here, decl, name);
  if (unused.isErr()) {
    this->reportTaken(here, name, unused.error());
    return;
  }
  here.imports.try_emplace(name.as, Checker::Import{target.value(), name.asLoc});
}

void ImportBinder::resolveImports(Module *module) {
  Checker::ModuleState &here = this->checker.state(module);
  for (const ImportDecl &decl : module->file.imports) {
    if (decl.path == "neverthrow") {
      this->checker.resultTypes().rejectNeverthrowImport(decl);
      for (const ModuleName &name : decl.names) {
        here.badImports.insert(name.as);
      }
      continue;
    }
    if (isBuiltinModule(decl.path) && !decl.from) {
      this->checker.builtinModuleChecker().importBuiltinModule(module, decl);
      continue;
    }
    ResultWholeImportOk whole = ImportBinder::wholeImport(decl);
    if (whole.isErr()) {
      this->checker.diags_.error(decl.wholeLoc, whole.error().message);
      here.badImports.insert(decl.whole);
    }
    for (const ModuleName &name : decl.names) {
      this->importName(here, decl, name);
    }
  }
}

void ImportBinder::bindImports() {
  Checker::ModuleState &here = this->checker.state(this->checker.module_);
  for (const auto &entry : here.nodeNames) {
    const Checker::NodeName &n = entry.getValue();
    ClassDecl *cls = nullptr;
    if (n.module == NodeModule::Fs && n.name == "Stats") {
      cls = this->checker.statsClass_;
    } else if (n.module == NodeModule::Fs && n.name == "Dirent") {
      cls = this->checker.direntClass_;
    }
    if (!cls) {
      continue;
    }
    this->checker.typeScopes_[0].try_emplace(entry.getKey(), cls);
    this->checker.scopes_[0].try_emplace(entry.getKey(), ScopeEntry{ClassEntry{.cls = cls}});
  }
  for (const auto &entry : here.imports) {
    llvm::StringRef alias = entry.getKey();
    const Checker::Binding &target = entry.getValue().target;
    Module *source = target.module;
    bool own = source == this->checker.module_;
    const llvm::StringMap<Stmt *> &types =
        own ? this->checker.typeScopes_[0] : this->checker.state(source).types;
    if (!this->checker.typeScopes_[0].count(alias)) {
      auto found = types.find(target.name);
      if (found != types.end()) {
        Stmt *type = found->second;
        this->checker.typeScopes_[0][alias] = type;
      }
    }
    const llvm::StringMap<ScopeEntry> &values =
        own ? this->checker.scopes_[0] : this->checker.state(source).values;
    if (this->checker.scopes_[0].count(alias)) {
      continue;
    }
    auto found = values.find(target.name);
    if (found == values.end()) {
      continue;
    }
    ScopeEntry value = found->second;
    if (!value.found()) {
      continue;
    }
    this->checker.scopes_[0][alias] = value;
    if (value.var() && !own) {
      here.importedVars.insert(value.var());
    }
  }
}

void ImportBinder::noteImportsInFunctions(SourceFile &program) {
  for (Module *module : program.modules) {
    for (const auto &entry : this->checker.state(module).imports) {
      if (this->checker.namesInFunctions_.contains(entry.getKey())) {
        this->checker.namesInFunctions_.insert(entry.getValue().target.name);
      }
    }
  }
}

FunctionDecl *ImportBinder::cycleCallee(FunctionDecl *fn, llvm::StringRef name) {
  Module *home = this->checker.declModules_.lookup(fn);
  if (!home) {
    return nullptr;
  }
  const llvm::StringMap<ScopeEntry> &values =
      home == this->checker.module_ ? this->checker.scopes_[0] : this->checker.state(home).values;
  auto found = values.find(name);
  if (found == values.end() || !found->second.func()) {
    return nullptr;
  }

  return found->second.func()->decl;
}

}
