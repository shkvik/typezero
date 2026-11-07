#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/import_binder/import_binder.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>

using llvm::dyn_cast;

namespace tz {

ModuleDeclarer::ModuleDeclarer(Checker &owner)
    : checker(owner) {}

void ModuleDeclarer::useModule(Module *module) {
  if (module == this->checker.module_) {
    return;
  }
  assert(this->checker.scopes_.size() == 1 && this->checker.typeScopes_.size() == 1 &&
         "a file changes only at the top level");
  if (this->checker.module_) {
    Checker::ModuleState &old = this->checker.state(this->checker.module_);
    old.values = this->checker.scopes_[0];
    old.types = this->checker.typeScopes_[0];
  }
  this->checker.module_ = module;
  this->checker.scopes_[0] = this->checker.state(module).values;
  this->checker.typeScopes_[0] = this->checker.state(module).types;
}

std::vector<FunctionDecl *> ModuleDeclarer::declareModules(SourceFile &program) {
  this->checker.moduleStates_.resize(program.modules.size());
  for (Module *module : program.modules) {
    Checker::ModuleState &here = this->checker.state(module);
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
    types[module->order] = this->checker.typeDeclarer().declareTypes(module->file.items, false);
    for (Stmt *type : types[module->order]) {
      this->checker.declModules_[type] = module;
    }
  }
  for (Module *module : program.modules) {
    this->checker.importBinder().resolveImports(module);
  }
  for (Module *module : program.modules) {
    this->useModule(module);
    this->checker.importBinder().bindImports();
  }
  for (Module *module : program.modules) {
    this->useModule(module);
    this->checker.typeDeclarer().resolveTypes(types[module->order]);
  }

  std::vector<FunctionDecl *> functions;
  for (Module *module : program.modules) {
    this->useModule(module);
    for (Stmt *item : module->file.items) {
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        this->checker.functionChecker().declareFunction(fn);
        this->checker.declModules_[fn] = module;
        functions.push_back(fn);
      }
      auto *cls = dyn_cast<ClassDecl>(item);
      if (!cls || !cls->type || cls->baseExpr) {
        continue;
      }
      this->checker.classDeclarer().declareMethods(cls);
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
        this->checker.declModules_[fn] = module;
        functions.push_back(fn);
      }
    }
  }
  for (Module *module : program.modules) {
    this->useModule(module);
    this->checker.importBinder().bindImports();
  }
  this->checker.methodsDeclared_ = true;
  for (size_t i = 0; i < this->checker.unfilled_.size(); ++i) {
    ClassDecl *instance = this->checker.unfilled_[i];
    this->checker.filling_.push_back(instance);
    this->checker.genericClassInstantiator().fillInstance(instance);
    this->checker.filling_.pop_back();
  }
  this->checker.unfilled_.clear();
  this->checker.lateClasses().markLateOverrides(program);
  this->checker.classLinks().linkClasses();
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

ModuleDeclarer::ResultExportUnique ModuleDeclarer::freeExport(const Checker::ModuleState &here,
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

void ModuleDeclarer::exportName(Checker::ModuleState &here, Module *module,
                                const ExportSource *source, const ModuleName &name) {
  if (source && !source->from) {
    return;
  }
  std::optional<Checker::Binding> target;
  if (source) {
    ResultExportTarget found = this->findExport(source->from, source->path, name);
    if (found.isErr()) {
      this->checker.diags_.error(name.loc, found.error().message);
      return;
    }
    target = found.value();
  } else {
    target = this->resolveLocal(module, name.name);
    ResultLocalIsKnown known = ModuleDeclarer::localExport(module, name, target.has_value());
    bool reported =
        known.isErr() && known.error().code == ModuleDeclarerLocalExportErrorCode::ALREADY_REPORTED;
    if (known.isErr() && !reported) {
      this->checker.diags_.error(name.loc, known.error().message);
    }
    if (known.isErr()) {
      return;
    }
  }
  ResultExportUnique unique = ModuleDeclarer::freeExport(here, name);
  if (unique.isErr()) {
    llvm::SMLoc exportedAt = here.exports.find(name.as)->second.loc;
    std::string previous = ErrorTextFreeExport::previous();
    this->checker.diags_.error(name.asLoc, unique.error().message);
    this->checker.diags_.note(exportedAt, previous);
    return;
  }
  here.exports.try_emplace(name.as, Checker::Export{*target, name.asLoc, nullptr});
}

ModuleDeclarer::ResultStarIsUnique ModuleDeclarer::starExport(llvm::StringRef name,
                                                              const Checker::Export *ours,
                                                              const ExportAll *all,
                                                              const Checker::Binding &target) {
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

const llvm::StringMap<Checker::Export> &ModuleDeclarer::exportsOf(Module *module) {
  Checker::ModuleState &here = this->checker.state(module);
  if (here.exportsDone || here.exportsBusy) {
    return here.exports;
  }
  here.exportsBusy = true;

  for (const ExportDecl &decl : module->file.exports) {
    const ExportSource *source = decl.source();
    ResultSourceUsable local = ModuleDeclarer::exportSource(source);
    if (local.isErr()) {
      this->checker.diags_.error(source->pathLoc, local.error().message);
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
      const Checker::Binding &target = entry.getValue().target;
      auto existing = here.exports.find(name);
      const Checker::Export *ours = existing != here.exports.end() ? &existing->second : nullptr;
      ResultStarIsUnique unique = ModuleDeclarer::starExport(name, ours, all, target);
      if (unique.isErr()) {
        this->checker.diags_.error(decl.loc, unique.error().message);
      }
      here.exports.try_emplace(name, Checker::Export{target, decl.loc, all});
    }
  }
  here.exportsBusy = false;
  here.exportsDone = true;

  return here.exports;
}

ModuleDeclarer::ResultExportTarget ModuleDeclarer::findExport(Module *from, llvm::StringRef path,
                                                              const ModuleName &name) {
  const llvm::StringMap<Checker::Export> &theirs = this->exportsOf(from);
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
  bool declared = missing && this->checker.state(from).declared.count(name.name);
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

std::optional<Checker::Binding> ModuleDeclarer::resolveLocal(Module *module, llvm::StringRef name) {
  if (this->checker.state(module).declared.count(name)) {
    return Checker::Binding{module, name};
  }
  for (const ImportDecl &decl : module->file.imports) {
    for (const ModuleName &imported : decl.names) {
      if (imported.as != name) {
        continue;
      }
      if (!decl.from) {
        return std::nullopt;
      }
      const llvm::StringMap<Checker::Export> &theirs = this->exportsOf(decl.from);
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
