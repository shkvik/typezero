#include "compiler/pipeline/check_program/declare/module_declarer/module_declarer.h"
#include "compiler/pipeline/check_program/declare/class_declarer/class_declarer.h"
#include "compiler/pipeline/check_program/declare/import_binder/import_binder.h"
#include "compiler/pipeline/check_program/declare/type_declarer/type_declarer.h"
#include "compiler/pipeline/check_program/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/pipeline/check_program/instances/late_classes/late_classes.h"
#include "compiler/pipeline/check_program/topics/classes/class_links/class_links.h"
#include "compiler/pipeline/check_program/topics/events/emitter_checker/emitter_checker.h"
#include "compiler/pipeline/check_program/topics/functions/function_checker/function_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>

using llvm::dyn_cast;

namespace tz {

ModuleDeclarer::ModuleDeclarer(CheckState &owner)
    : state(owner) {}

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

std::vector<FunctionDecl *> ModuleDeclarer::declareModules(SourceFile &program) {
  this->state.getModuleStates().prepareModules(int(program.modules.size()));
  for (Module *module : program.modules) {
    ModuleState &here = this->state.getModuleStates().getModuleState(module);
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

  std::vector<std::vector<Stmt *>> types(program.modules.size());
  for (Module *module : program.modules) {
    this->useModule(module);
    types[module->order] = TypeDeclarer(this->state).declareTypes(module->file.items, false);
    for (Stmt *type : types[module->order]) {
      this->state.getModuleStates().rememberDeclModule(type, module);
    }
  }
  for (Module *module : program.modules) {
    ImportBinder(this->state).resolveImports(module);
  }
  for (Module *module : program.modules) {
    this->useModule(module);
    ImportBinder(this->state).bindImports();
  }
  for (Module *module : program.modules) {
    this->useModule(module);
    TypeDeclarer(this->state).resolveTypes(types[module->order]);
  }

  std::vector<FunctionDecl *> functions;
  for (Module *module : program.modules) {
    this->useModule(module);
    for (Stmt *item : module->file.items) {
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        FunctionChecker(this->state).declareFunction(fn);
        this->state.getModuleStates().rememberDeclModule(fn, module);
        functions.push_back(fn);
      }
      auto *cls = dyn_cast<ClassDecl>(item);
      if (!cls || !cls->type || cls->baseExpr) {
        continue;
      }
      ClassDeclarer(this->state).declareMethods(cls);
      std::vector<FunctionDecl *> methods = cls->methods;
      methods.insert(methods.end(), cls->constructors.begin(), cls->constructors.end());
      if (cls->init) {
        methods.push_back(cls->init);
      }
      for (const ClassField &f : cls->fields) {
        if (f.method()) {
          methods.push_back(f.method());
        }
      }
      for (FunctionDecl *fn : methods) {
        this->state.getModuleStates().rememberDeclModule(fn, module);
        functions.push_back(fn);
      }
    }
  }
  for (Module *module : program.modules) {
    this->useModule(module);
    ImportBinder(this->state).bindImports();
  }
  this->state.getClassStore().markMethodsDeclared();
  GenericStore &generics = this->state.getGenericStore();
  for (ClassDecl *instance : generics.takeUnfilledClasses()) {
    generics.enterFilling(instance);
    GenericClassInstantiator(this->state).fillInstance(instance);
    generics.leaveFilling();
  }
  LateClasses(this->state).markLateOverrides(program);
  ClassLinks(this->state).linkClasses();
  for (Module *module : program.modules) {
    this->exportsOf(module);
  }

  return functions;
}

ModuleDeclarer::ResultSourceUsable ModuleDeclarer::exportSource(const ExportSource *source) {
  if (source && source->path == "neverthrow") {
    std::string message = ErrorTextExportSource::neverthrow();

    return err(ModuleDeclarerExportSourceError{
        .code = ModuleDeclarerExportSourceErrorCode::NEVERTHROW,
        .message = message,
    });
  }
  if (source && isBuiltinModule(source->path)) {
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
    return isBuiltinModule(i.path) && (i.whole == name.name || llvm::any_of(i.names, sameAlias));
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

void ModuleDeclarer::exportName(ModuleState &here, Module *module, const ExportSource *source,
                                const ModuleName &name) {
  if (source && !source->from) {
    return;
  }
  std::optional<ModuleBinding> target;
  if (source) {
    ResultExportTarget found = this->findExport(source->from, source->path, name);
    if (found.isErr()) {
      this->state.getDiagnostics().error(name.loc, found.error().message);
      return;
    }
    target = found.value();
  } else {
    target = this->resolveLocal(module, name.name);
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
  ModuleState &here = this->state.getModuleStates().getModuleState(module);
  if (here.exportsDone || here.exportsBusy) {
    return here.exports;
  }
  here.exportsBusy = true;

  for (const ExportDecl &decl : module->file.exports) {
    const ExportSource *source = decl.source();
    ResultSourceUsable local = ModuleDeclarer::exportSource(source);
    if (local.isErr()) {
      this->state.getDiagnostics().error(source->pathLoc, local.error().message);
      continue;
    }
    for (const ModuleName &name : decl.names()) {
      this->exportName(here, module, source, name);
    }
  }

  for (const ExportDecl &decl : module->file.exports) {
    const ExportAll *all = decl.form.as<ExportAll>();
    if (!all || !all->source.from || all->source.from == module) {
      continue;
    }
    for (const auto &entry : this->exportsOf(all->source.from)) {
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
  here.exportsBusy = false;
  here.exportsDone = true;

  return here.exports;
}

ModuleDeclarer::ResultExportTarget ModuleDeclarer::findExport(Module *from, llvm::StringRef path,
                                                              const ModuleName &name) {
  const llvm::StringMap<ModuleExport> &theirs = this->exportsOf(from);
  auto found = theirs.find(name.name);
  bool missing = found == theirs.end();
  if (missing && from->builtin) {
    std::string hint = EmitterChecker::libraryExportHint(name.name);
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

std::optional<ModuleBinding> ModuleDeclarer::resolveLocal(Module *module, llvm::StringRef name) {
  if (this->state.getModuleStates().getModuleState(module).declared.count(name)) {
    return ModuleBinding{module, name};
  }
  for (const ImportDecl &decl : module->file.imports) {
    for (const ModuleName &imported : decl.names) {
      if (imported.as != name) {
        continue;
      }
      if (!decl.from) {
        return std::nullopt;
      }
      const llvm::StringMap<ModuleExport> &theirs = this->exportsOf(decl.from);
      auto found = theirs.find(imported.name);
      if (found == theirs.end()) {
        return std::nullopt;
      }
      return found->second.target;
    }
  }

  return std::nullopt;
}

}
