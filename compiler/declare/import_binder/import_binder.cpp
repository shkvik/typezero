#include "compiler/declare/import_binder/import_binder.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/result/result_types/result_types.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

ImportBinder::ImportBinder(Sema &sema) : sema_(sema) {}

void ImportBinder::resolveImports(Module *module) {
  Sema::ModuleState &here = sema_.state(module);
  for (const ImportDecl &decl : module->file.imports) {
    if (decl.path == "neverthrow") {
      sema_.resultTypes().importNeverthrow(module, decl);
      continue;
    }
    // A built-in module written in tz (Module::builtin) is imported as any
    // file is.
    if (isBuiltinModule(decl.path) && !decl.from) {
      sema_.builtinModuleChecker().importBuiltinModule(module, decl);
      continue;
    }
    // `import events from "node:events"`, which the parser leaves to Sema
    // for the modules built in.
    if (!decl.whole.empty() && decl.from && decl.from->builtin) {
      sema_.diags_.error(decl.wholeLoc,
                   std::string(decl.wholeIsDefault ? "default imports are not supported: there is "
                                                     "no 'export default'"
                                                   : "'import * as' is not supported") +
                       "; import the names you need, e.g. 'import { EventEmitter, "
                       "once } from \"" +
                       decl.path + "\"'");
      here.badImports.insert(decl.whole);
    }
    for (const ModuleName &name : decl.names) {
      if (!decl.from) {
        here.badImports.insert(name.as); // the file is not loaded: reported
        continue;
      }
      std::optional<Sema::Binding> target = sema_.moduleDeclarer().findExport(decl.from, decl.path, name);
      if (!target) {
        here.badImports.insert(name.as);
        continue;
      }
      if (auto own = here.declared.find(name.as); own != here.declared.end()) {
        sema_.diags_.error(name.asLoc,
                     "import '" + name.as + "' conflicts with the declaration of '" + name.as +
                         "' in this file; import it under another name, e.g. 'import { " +
                         name.name + " as " + name.name + "2 } from \"" + decl.path + "\"'");
        sema_.diags_.note(own->second, "'" + name.as + "' is declared here");
        continue;
      }
      auto [it, inserted] = here.imports.try_emplace(name.as, Sema::Import{*target, name.asLoc});
      if (!inserted) {
        sema_.diags_.error(name.asLoc, "duplicate import of '" + name.as + "'");
        sema_.diags_.note(it->second.loc, "previously imported here");
      }
    }
  }
}

void ImportBinder::bindImports() {
  Sema::ModuleState &here = sema_.state(sema_.module_);
  // The classes of node:fs, as types and for `instanceof`.
  for (const auto &entry : here.nodeNames) {
    const Sema::NodeName &n = entry.getValue();
    ClassDecl *cls = n.module != NodeModule::Fs ? nullptr
                     : n.name == "Stats"        ? sema_.statsClass_
                     : n.name == "Dirent"       ? sema_.direntClass_
                                                : nullptr;
    if (!cls) {
      continue;
    }
    sema_.typeScopes_[0].try_emplace(entry.getKey(), cls);
    sema_.scopes_[0].try_emplace(entry.getKey(), Sema::ScopeEntry{nullptr, nullptr, nullptr, cls});
  }
  for (const auto &entry : here.imports) {
    llvm::StringRef alias = entry.getKey();
    const Sema::Binding &target = entry.getValue().target;
    Module *source = target.module;
    // As a type.
    const llvm::StringMap<Stmt *> &types = source == sema_.module_ ? sema_.typeScopes_[0] : sema_.state(source).types;
    if (!sema_.typeScopes_[0].count(alias)) {
      if (auto found = types.find(target.name); found != types.end()) {
        Stmt *type = found->second;
        sema_.typeScopes_[0][alias] = type;
      }
    }
    // As a value: a function or an enum, or a variable once the code of its
    // file has declared it.
    const llvm::StringMap<Sema::ScopeEntry> &values =
        source == sema_.module_ ? sema_.scopes_[0] : sema_.state(source).values;
    if (sema_.scopes_[0].count(alias)) {
      continue;
    }
    auto found = values.find(target.name);
    if (found == values.end()) {
      continue;
    }
    Sema::ScopeEntry value = found->second;
    if (!value.var && !value.func && !value.enumDecl && !value.cls) {
      continue;
    }
    sema_.scopes_[0][alias] = value;
    if (value.var && source != sema_.module_) {
      here.importedVars.insert(value.var);
    }
  }
}

void ImportBinder::noteImportsInFunctions(SourceFile &program) {
  for (Module *module : program.modules) {
    for (const auto &entry : sema_.state(module).imports) {
      if (sema_.namesInFunctions_.contains(entry.getKey())) {
        sema_.namesInFunctions_.insert(entry.getValue().target.name);
      }
    }
  }
}

FunctionDecl *ImportBinder::cycleCallee(FunctionDecl *fn, llvm::StringRef name) {
  Module *home = sema_.declModules_.lookup(fn);
  if (!home) {
    return nullptr;
  }
  const llvm::StringMap<Sema::ScopeEntry> &values = home == sema_.module_ ? sema_.scopes_[0] : sema_.state(home).values;
  auto found = values.find(name);
  if (found == values.end() || !found->second.func) {
    return nullptr;
  }

  return found->second.func->decl;
}

} // namespace tz
