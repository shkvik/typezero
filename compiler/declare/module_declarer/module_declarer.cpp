#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/declare/import_binder/import_binder.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

ModuleDeclarer::ModuleDeclarer(Sema &sema) : sema_(sema) {}

void ModuleDeclarer::useModule(Module *module) {
  if (module == sema_.module_) {
    return;
  }
  assert(sema_.scopes_.size() == 1 && sema_.typeScopes_.size() == 1 && "a file changes only at the top level");
  if (sema_.module_) {
    Sema::ModuleState &old = sema_.state(sema_.module_);
    old.values = sema_.scopes_[0];
    old.types = sema_.typeScopes_[0];
  }
  sema_.module_ = module;
  sema_.scopes_[0] = sema_.state(module).values;
  sema_.typeScopes_[0] = sema_.state(module).types;
}

std::vector<FunctionDecl *> ModuleDeclarer::declareModules(SourceFile &program) {
  sema_.moduleStates_.resize(program.modules.size());
  for (Module *module : program.modules) {
    Sema::ModuleState &here = sema_.state(module);
    auto declared = [&](llvm::StringRef name, llvm::SMLoc loc) {
      if (!name.empty()) {
        here.declared.try_emplace(name, loc);
      }
    };
    for (Stmt *item : module->file.items) {
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        declared(fn->name, fn->loc());
      } else if (auto *alias = dyn_cast<TypeAliasDecl>(item)) {
        declared(alias->name, alias->loc());
      } else if (auto *e = dyn_cast<EnumDecl>(item)) {
        declared(e->name, e->loc());
      } else if (auto *cls = dyn_cast<ClassDecl>(item)) {
        declared(cls->name, cls->loc());
      } else if (auto *decl = dyn_cast<VarDeclStmt>(item)) {
        for (const VarDeclarator &d : decl->decls) {
          declared(d.name, d.loc);
          here.topLevelNames.insert(d.name);
        }
      }
    }
  }

  // The names of the types first, then what files import, and only then
  // what the types are: a type may use the types of other files.
  std::vector<std::vector<Stmt *>> types(program.modules.size());
  for (Module *module : program.modules) {
    useModule(module);
    types[module->order] = sema_.typeDeclarer().declareTypes(module->file.items, /*resolve=*/false);
    for (Stmt *type : types[module->order]) {
      sema_.declModules_[type] = module;
    }
  }
  for (Module *module : program.modules) {
    sema_.importBinder().resolveImports(module);
  }
  for (Module *module : program.modules) {
    useModule(module);
    sema_.importBinder().bindImports();
  }
  for (Module *module : program.modules) {
    useModule(module);
    sema_.typeDeclarer().resolveTypes(types[module->order]);
  }

  std::vector<FunctionDecl *> functions;
  for (Module *module : program.modules) {
    useModule(module);
    for (Stmt *item : module->file.items) {
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        sema_.functionChecker().declareFunction(fn);
        sema_.declModules_[fn] = module;
        functions.push_back(fn);
      }
      // The methods of a class are functions of its file. A class whose
      // base is a value gets them where it is declared (see makeLateClass).
      auto *cls = dyn_cast<ClassDecl>(item);
      if (!cls || !cls->type || cls->baseExpr) {
        continue;
      }
      sema_.classDeclarer().declareMethods(cls);
      std::vector<FunctionDecl *> methods = cls->methods;
      methods.insert(methods.end(), cls->constructors.begin(), cls->constructors.end());
      if (cls->init) {
        methods.push_back(cls->init);
      }
      for (const ClassField &f : cls->fields) {
        if (f.method) {
          methods.push_back(f.method);
        }
      }
      for (FunctionDecl *fn : methods) {
        sema_.declModules_[fn] = module;
        functions.push_back(fn);
      }
    }
  }
  for (Module *module : program.modules) {
    useModule(module);
    sema_.importBinder().bindImports();
  }
  // The instances of generic classes that types named so far, now that the
  // generic classes have their methods.
  sema_.methodsDeclared_ = true;
  for (size_t i = 0; i < sema_.unfilled_.size(); ++i) {
    sema_.filling_.push_back(sema_.unfilled_[i]);
    sema_.genericClassInstantiator().fillInstance(sema_.unfilled_[i]);
    sema_.filling_.pop_back();
  }
  sema_.unfilled_.clear();
  // Which methods override which, now that every class has its methods; a
  // class that a class expression makes later may override some too.
  sema_.lateClasses().markLateOverrides(program);
  sema_.classLinks().linkClasses();
  // The exports nothing imports are checked too.
  for (Module *module : program.modules) {
    exportsOf(module);
  }

  return functions;
}

const llvm::StringMap<Sema::Export> &ModuleDeclarer::exportsOf(Module *module) {
  Sema::ModuleState &here = sema_.state(module);
  // Busy: the exports of files that re-export each other with `export *`,
  // which adds nothing more, as in JavaScript.
  if (here.exportsDone || here.exportsBusy) {
    return here.exports;
  }
  here.exportsBusy = true;

  for (const ExportDecl &decl : module->file.exports) {
    if (decl.path == "neverthrow") {
      sema_.diags_.error(decl.pathLoc, "neverthrow need not be re-exported: its names are built in, and "
                                 "every file has them");
      continue;
    }
    if (isBuiltinModule(decl.path)) {
      sema_.diags_.error(decl.pathLoc, "a built-in module is not re-exported: import what a file needs "
                                 "from '" +
                                     decl.path + "' in that file");
      continue;
    }
    if (decl.all) {
      continue;
    }
    for (const ModuleName &name : decl.names) {
      std::optional<Sema::Binding> target;
      if (!decl.path.empty()) {
        // `export { a } from "./x"`
        if (!decl.from) {
          continue; // not loaded: reported
        }
        target = findExport(decl.from, decl.path, name);
        if (!target) {
          continue;
        }
      } else {
        target = resolveLocal(module, name.name);
        bool builtin = llvm::any_of(module->file.imports, [&](const ImportDecl &i) {
          return isBuiltinModule(i.path) &&
                 (i.whole == name.name ||
                  llvm::any_of(i.names, [&](const ModuleName &n) { return n.as == name.name; }));
        });
        if (!target && builtin) {
          sema_.diags_.error(name.loc, "'" + name.name +
                                     "' comes from a built-in module, which is not "
                                     "re-exported: import it from there in the file that uses it");
          continue;
        }
        bool imported = llvm::any_of(module->file.imports, [&](const ImportDecl &i) {
          return llvm::any_of(i.names, [&](const ModuleName &n) { return n.as == name.name; });
        });
        if (!target && !imported) { // a failed import is reported at the import
          sema_.diags_.error(name.loc, "cannot find '" + name.name +
                                     "' to export: this file neither declares nor imports it");
        }
        if (!target) {
          continue;
        }
      }
      auto [it, inserted] = here.exports.try_emplace(name.as, Sema::Export{*target, name.asLoc, nullptr});
      if (!inserted) {
        sema_.diags_.error(name.asLoc, "duplicate export of '" + name.as + "'");
        sema_.diags_.note(it->second.loc, "previously exported here");
      }
    }
  }

  // `export *` passes on what the other file exports, except the names this
  // file exports itself. Two files that export one name to different things
  // make it ambiguous, as in tsc.
  for (const ExportDecl &decl : module->file.exports) {
    if (!decl.all || !decl.from || decl.from == module) {
      continue;
    }
    for (const auto &entry : exportsOf(decl.from)) {
      llvm::StringRef name = entry.getKey();
      const Sema::Binding &target = entry.getValue().target;
      auto [it, inserted] = here.exports.try_emplace(name, Sema::Export{target, decl.loc, &decl});
      const Sema::Export &ours = it->second;
      if (inserted || !ours.all || ours.target == target) {
        continue;
      }
      sema_.diags_.error(decl.loc, "'" + name + "' is exported both by '" + ours.all->path +
                                 "' and by '" + decl.path +
                                 "' through 'export *'; re-export the one you mean by name, e.g. "
                                 "'export { " +
                                 name + " } from \"" + ours.all->path + "\"'");
    }
  }
  here.exportsBusy = false;
  here.exportsDone = true;

  return here.exports;
}

std::optional<Sema::Binding> ModuleDeclarer::findExport(Module *from, llvm::StringRef path,
                                              const ModuleName &name) {
  const llvm::StringMap<Sema::Export> &theirs = exportsOf(from);
  if (auto found = theirs.find(name.name); found != theirs.end()) {
    return found->second.target;
  }
  std::string message = "'" + path.str() + "' has no export named '" + name.name.str() + "'";
  if (from->builtin) {
    message += EmitterChecker::libraryExportHint(name.name);
  } else if (sema_.state(from).declared.count(name.name)) {
    message += "; add 'export' to its declaration in " + from->name;
  }
  sema_.diags_.error(name.loc, message);

  return std::nullopt;
}

std::optional<Sema::Binding> ModuleDeclarer::resolveLocal(Module *module, llvm::StringRef name) {
  if (sema_.state(module).declared.count(name)) {
    return Sema::Binding{module, name};
  }
  for (const ImportDecl &decl : module->file.imports) {
    for (const ModuleName &imported : decl.names) {
      if (imported.as != name) {
        continue;
      }
      if (!decl.from) {
        return std::nullopt;
      }
      const llvm::StringMap<Sema::Export> &theirs = exportsOf(decl.from);
      auto found = theirs.find(imported.name);
      if (found == theirs.end()) {
        return std::nullopt;
      }
      return found->second.target;
    }
  }

  return std::nullopt;
}

} // namespace tz
