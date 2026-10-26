#include "compiler/declare/import_binder/import_binder.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/result/result_types/result_types.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

ImportBinder::ImportBinder(Checker &checker) : checker_(checker) {}

void ImportBinder::resolveImports(Module *module) {
  Checker::ModuleState &here = checker_.state(module);
  for (const ImportDecl &decl : module->file.imports) {
    if (decl.path == "neverthrow") {
      checker_.resultTypes().importNeverthrow(module, decl);
      continue;
    }
    if (isBuiltinModule(decl.path) && !decl.from) {
      checker_.builtinModuleChecker().importBuiltinModule(module, decl);
      continue;
    }
    if (!decl.whole.empty() && decl.from && decl.from->builtin) {
      checker_.diags_.error(decl.wholeLoc,
                            std::string(decl.wholeIsDefault
                                            ? "default imports are not supported: there is "
                                              "no 'export default'"
                                            : "'import * as' is not supported") +
                                "; import the names you need, e.g. 'import { EventEmitter, "
                                "once } from \"" +
                                decl.path + "\"'");
      here.badImports.insert(decl.whole);
    }
    for (const ModuleName &name : decl.names) {
      if (!decl.from) {
        here.badImports.insert(name.as);
        continue;
      }
      std::optional<Checker::Binding> target =
          checker_.moduleDeclarer().findExport(decl.from, decl.path, name);
      if (!target) {
        here.badImports.insert(name.as);
        continue;
      }
      if (auto own = here.declared.find(name.as); own != here.declared.end()) {
        checker_.diags_.error(
            name.asLoc, "import '" + name.as + "' conflicts with the declaration of '" + name.as +
                            "' in this file; import it under another name, e.g. 'import { " +
                            name.name + " as " + name.name + "2 } from \"" + decl.path + "\"'");
        checker_.diags_.note(own->second, "'" + name.as + "' is declared here");
        continue;
      }
      auto [it, inserted] = here.imports.try_emplace(name.as, Checker::Import{*target, name.asLoc});
      if (!inserted) {
        checker_.diags_.error(name.asLoc, "duplicate import of '" + name.as + "'");
        checker_.diags_.note(it->second.loc, "previously imported here");
      }
    }
  }
}

void ImportBinder::bindImports() {
  Checker::ModuleState &here = checker_.state(checker_.module_);
  for (const auto &entry : here.nodeNames) {
    const Checker::NodeName &n = entry.getValue();
    ClassDecl *cls = n.module != NodeModule::Fs ? nullptr
                     : n.name == "Stats"        ? checker_.statsClass_
                     : n.name == "Dirent"       ? checker_.direntClass_
                                                : nullptr;
    if (!cls) {
      continue;
    }
    checker_.typeScopes_[0].try_emplace(entry.getKey(), cls);
    checker_.scopes_[0].try_emplace(entry.getKey(),
                                    Checker::ScopeEntry{nullptr, nullptr, nullptr, cls});
  }
  for (const auto &entry : here.imports) {
    llvm::StringRef alias = entry.getKey();
    const Checker::Binding &target = entry.getValue().target;
    Module *source = target.module;
    const llvm::StringMap<Stmt *> &types =
        source == checker_.module_ ? checker_.typeScopes_[0] : checker_.state(source).types;
    if (!checker_.typeScopes_[0].count(alias)) {
      if (auto found = types.find(target.name); found != types.end()) {
        Stmt *type = found->second;
        checker_.typeScopes_[0][alias] = type;
      }
    }
    const llvm::StringMap<Checker::ScopeEntry> &values =
        source == checker_.module_ ? checker_.scopes_[0] : checker_.state(source).values;
    if (checker_.scopes_[0].count(alias)) {
      continue;
    }
    auto found = values.find(target.name);
    if (found == values.end()) {
      continue;
    }
    Checker::ScopeEntry value = found->second;
    if (!value.var && !value.func && !value.enumDecl && !value.cls) {
      continue;
    }
    checker_.scopes_[0][alias] = value;
    if (value.var && source != checker_.module_) {
      here.importedVars.insert(value.var);
    }
  }
}

void ImportBinder::noteImportsInFunctions(SourceFile &program) {
  for (Module *module : program.modules) {
    for (const auto &entry : checker_.state(module).imports) {
      if (checker_.namesInFunctions_.contains(entry.getKey())) {
        checker_.namesInFunctions_.insert(entry.getValue().target.name);
      }
    }
  }
}

FunctionDecl *ImportBinder::cycleCallee(FunctionDecl *fn, llvm::StringRef name) {
  Module *home = checker_.declModules_.lookup(fn);
  if (!home) {
    return nullptr;
  }
  const llvm::StringMap<Checker::ScopeEntry> &values =
      home == checker_.module_ ? checker_.scopes_[0] : checker_.state(home).values;
  auto found = values.find(name);
  if (found == values.end() || !found->second.func) {
    return nullptr;
  }

  return found->second.func->decl;
}

}
