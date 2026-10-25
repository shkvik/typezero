// Modules: the files of a program (docs/modules.md).
//
// Each file has a top-level scope of its own: what it declares, and what it
// imports. An import or an export names a top-level declaration of some file
// (a Binding), through any number of re-exports. Types and functions of every
// file are declared before any code is checked, so they do not depend on the
// order files load in. Top-level code is checked file by file in that order,
// the order the program runs it in: a file sees the variables of the files
// that load before it. In an import cycle a file loads before a file it
// imports; reading a variable of that file while loading is an error, as is
// calling a function that reads a variable before the code that sets it runs
// (checkLoadOrder).

#include "compiler/check/sema/sema.h"

#include "llvm/ADT/STLExtras.h"

#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// The function a callee or a callback names directly, if any.
const FunctionDecl *directFunction(const Expr *expr) {
  while (auto *cast = dyn_cast<CastExpr>(expr))
    expr = cast->operand;
  if (auto *fe = dyn_cast<FunctionExpr>(expr))
    return fe->fn;
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    if (name->func)
      return name->func->decl;
    if (const VarSymbol *var = name->var)
      return var->function ? var->function : var->isConst ? var->lambda : nullptr;
  }
  return nullptr;
}

// The functions a call runs before it returns: the function it calls, and
// the callbacks a built-in such as map or forEach calls.
void calledFunctions(const CallExpr *call, llvm::SmallVectorImpl<const FunctionDecl *> &called) {
  if (call->func && call->func->decl)
    called.push_back(call->func->decl);
  else if (call->method)
    called.push_back(call->method->decl);
  for (const CallExpr::UnionMethod &m : call->unionMethods)
    if (m.method)
      called.push_back(m.method->decl);
  else if (const FunctionDecl *fn = call->callee ? directFunction(call->callee) : nullptr)
    called.push_back(fn); // `(() => ...)()`
  if ((call->builtin >= Builtin::ArrForEach && call->builtin <= Builtin::ArrSortWith) ||
      isResultCallback(call->builtin) || call->builtin == Builtin::SafeTry)
    for (const Expr *arg : call->args)
      if (const FunctionDecl *fn = directFunction(arg))
        called.push_back(fn);
}

// What `new C()` runs: the initializers of the fields and the constructor.
// The call of the constructor stands for it.
const CallExpr *constructorCall(const Expr *expr,
                                llvm::SmallVectorImpl<const FunctionDecl *> &called) {
  auto *create = dyn_cast<NewExpr>(expr);
  if (!create || !create->cls || !create->ctor)
    return nullptr;
  if (create->cls->init)
    called.push_back(create->cls->init);
  if (create->ctor->func)
    called.push_back(create->ctor->func->decl);
  return called.empty() ? nullptr : create->ctor;
}

// A function in messages: "'f'", or what a class runs.
std::string calleeName(const FunctionDecl *fn) {
  if (!fn->cls)
    return quotedName(fn->name);
  if (fn == fn->cls->init || fn->isConstructor)
    return "'new " + fn->cls->name.str() + "()'";
  return "'" + fn->cls->name.str() + "." + fn->name.str() + "'";
}

} // namespace

//===----------------------------------------------------------------------===//
// Scopes of files
//===----------------------------------------------------------------------===//

void Sema::useModule(Module *module) {
  if (module == module_)
    return;
  assert(scopes_.size() == 1 && typeScopes_.size() == 1 && "a file changes only at the top level");
  if (module_) {
    ModuleState &old = state(module_);
    old.values = scopes_[0];
    old.types = typeScopes_[0];
  }
  module_ = module;
  scopes_[0] = state(module).values;
  typeScopes_[0] = state(module).types;
}

std::vector<FunctionDecl *> Sema::declareModules(SourceFile &program) {
  moduleStates_.resize(program.modules.size());
  for (Module *module : program.modules) {
    ModuleState &here = state(module);
    auto declared = [&](llvm::StringRef name, llvm::SMLoc loc) {
      if (!name.empty())
        here.declared.try_emplace(name, loc);
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
    types[module->order] = declareTypes(module->file.items, /*resolve=*/false);
    for (Stmt *type : types[module->order])
      declModules_[type] = module;
  }
  for (Module *module : program.modules)
    resolveImports(module);
  for (Module *module : program.modules) {
    useModule(module);
    bindImports();
  }
  for (Module *module : program.modules) {
    useModule(module);
    resolveTypes(types[module->order]);
  }

  std::vector<FunctionDecl *> functions;
  for (Module *module : program.modules) {
    useModule(module);
    for (Stmt *item : module->file.items) {
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        declareFunction(fn);
        declModules_[fn] = module;
        functions.push_back(fn);
      }
      // The methods of a class are functions of its file. A class whose
      // base is a value gets them where it is declared (see makeLateClass).
      auto *cls = dyn_cast<ClassDecl>(item);
      if (!cls || !cls->type || cls->baseExpr)
        continue;
      declareMethods(cls);
      std::vector<FunctionDecl *> methods = cls->methods;
      methods.insert(methods.end(), cls->constructors.begin(), cls->constructors.end());
      if (cls->init)
        methods.push_back(cls->init);
      for (const ClassField &f : cls->fields)
        if (f.method)
          methods.push_back(f.method);
      for (FunctionDecl *fn : methods) {
        declModules_[fn] = module;
        functions.push_back(fn);
      }
    }
  }
  for (Module *module : program.modules) {
    useModule(module);
    bindImports();
  }
  // The instances of generic classes that types named so far, now that the
  // generic classes have their methods.
  methodsDeclared_ = true;
  for (size_t i = 0; i < unfilled_.size(); ++i) {
    filling_.push_back(unfilled_[i]);
    fillInstance(unfilled_[i]);
    filling_.pop_back();
  }
  unfilled_.clear();
  // Which methods override which, now that every class has its methods; a
  // class that a class expression makes later may override some too.
  markLateOverrides(program);
  linkClasses();
  // The exports nothing imports are checked too.
  for (Module *module : program.modules)
    exportsOf(module);
  return functions;
}

//===----------------------------------------------------------------------===//
// Imports and exports
//===----------------------------------------------------------------------===//

const llvm::StringMap<Sema::Export> &Sema::exportsOf(Module *module) {
  ModuleState &here = state(module);
  // Busy: the exports of files that re-export each other with `export *`,
  // which adds nothing more, as in JavaScript.
  if (here.exportsDone || here.exportsBusy)
    return here.exports;
  here.exportsBusy = true;

  for (const ExportDecl &decl : module->file.exports) {
    if (decl.path == "neverthrow") {
      diags_.error(decl.pathLoc, "neverthrow need not be re-exported: its names are built in, and "
                                 "every file has them");
      continue;
    }
    if (isBuiltinModule(decl.path)) {
      diags_.error(decl.pathLoc, "a built-in module is not re-exported: import what a file needs "
                                 "from '" + decl.path + "' in that file");
      continue;
    }
    if (decl.all)
      continue;
    for (const ModuleName &name : decl.names) {
      std::optional<Binding> target;
      if (!decl.path.empty()) {
        // `export { a } from "./x"`
        if (!decl.from)
          continue; // not loaded: reported
        target = findExport(decl.from, decl.path, name);
        if (!target)
          continue;
      } else {
        target = resolveLocal(module, name.name);
        bool builtin = llvm::any_of(module->file.imports, [&](const ImportDecl &i) {
          return isBuiltinModule(i.path) &&
                 (i.whole == name.name ||
                  llvm::any_of(i.names, [&](const ModuleName &n) { return n.as == name.name; }));
        });
        if (!target && builtin) {
          diags_.error(name.loc, "'" + name.name + "' comes from a built-in module, which is not "
                                 "re-exported: import it from there in the file that uses it");
          continue;
        }
        bool imported = llvm::any_of(module->file.imports, [&](const ImportDecl &i) {
          return llvm::any_of(i.names, [&](const ModuleName &n) { return n.as == name.name; });
        });
        if (!target && !imported) // a failed import is reported at the import
          diags_.error(name.loc, "cannot find '" + name.name +
                                     "' to export: this file neither declares nor imports it");
        if (!target)
          continue;
      }
      auto [it, inserted] = here.exports.try_emplace(name.as, Export{*target, name.asLoc, nullptr});
      if (!inserted) {
        diags_.error(name.asLoc, "duplicate export of '" + name.as + "'");
        diags_.note(it->second.loc, "previously exported here");
      }
    }
  }

  // `export *` passes on what the other file exports, except the names this
  // file exports itself. Two files that export one name to different things
  // make it ambiguous, as in tsc.
  for (const ExportDecl &decl : module->file.exports) {
    if (!decl.all || !decl.from || decl.from == module)
      continue;
    for (const auto &entry : exportsOf(decl.from)) {
      llvm::StringRef name = entry.getKey();
      const Binding &target = entry.getValue().target;
      auto [it, inserted] = here.exports.try_emplace(name, Export{target, decl.loc, &decl});
      const Export &ours = it->second;
      if (inserted || !ours.all || ours.target == target)
        continue;
      diags_.error(decl.loc, "'" + name + "' is exported both by '" + ours.all->path +
                                 "' and by '" + decl.path +
                                 "' through 'export *'; re-export the one you mean by name, e.g. "
                                 "'export { " + name + " } from \"" + ours.all->path + "\"'");
    }
  }
  here.exportsBusy = false;
  here.exportsDone = true;
  return here.exports;
}

std::optional<Sema::Binding> Sema::findExport(Module *from, llvm::StringRef path,
                                              const ModuleName &name) {
  const llvm::StringMap<Export> &theirs = exportsOf(from);
  if (auto found = theirs.find(name.name); found != theirs.end())
    return found->second.target;
  std::string message = "'" + path.str() + "' has no export named '" + name.name.str() + "'";
  if (from->builtin)
    message += libraryExportHint(name.name);
  else if (state(from).declared.count(name.name))
    message += "; add 'export' to its declaration in " + from->name;
  diags_.error(name.loc, message);
  return std::nullopt;
}

std::optional<Sema::Binding> Sema::resolveLocal(Module *module, llvm::StringRef name) {
  if (state(module).declared.count(name))
    return Binding{module, name};
  for (const ImportDecl &decl : module->file.imports)
    for (const ModuleName &imported : decl.names) {
      if (imported.as != name)
        continue;
      if (!decl.from)
        return std::nullopt;
      const llvm::StringMap<Export> &theirs = exportsOf(decl.from);
      auto found = theirs.find(imported.name);
      if (found == theirs.end())
        return std::nullopt;
      return found->second.target;
    }
  return std::nullopt;
}

void Sema::resolveImports(Module *module) {
  ModuleState &here = state(module);
  for (const ImportDecl &decl : module->file.imports) {
    if (decl.path == "neverthrow") {
      importNeverthrow(module, decl);
      continue;
    }
    // A built-in module written in tz (Module::builtin) is imported as any
    // file is.
    if (isBuiltinModule(decl.path) && !decl.from) {
      importBuiltinModule(module, decl);
      continue;
    }
    // `import events from "node:events"`, which the parser leaves to Sema
    // for the modules built in.
    if (!decl.whole.empty() && decl.from && decl.from->builtin) {
      diags_.error(decl.wholeLoc, std::string(decl.wholeIsDefault
                                                  ? "default imports are not supported: there is "
                                                    "no 'export default'"
                                                  : "'import * as' is not supported") +
                                      "; import the names you need, e.g. 'import { EventEmitter, "
                                      "once } from \"" + decl.path + "\"'");
      here.badImports.insert(decl.whole);
    }
    for (const ModuleName &name : decl.names) {
      if (!decl.from) {
        here.badImports.insert(name.as); // the file is not loaded: reported
        continue;
      }
      std::optional<Binding> target = findExport(decl.from, decl.path, name);
      if (!target) {
        here.badImports.insert(name.as);
        continue;
      }
      if (auto own = here.declared.find(name.as); own != here.declared.end()) {
        diags_.error(name.asLoc,
                     "import '" + name.as + "' conflicts with the declaration of '" + name.as +
                         "' in this file; import it under another name, e.g. 'import { " +
                         name.name + " as " + name.name + "2 } from \"" + decl.path + "\"'");
        diags_.note(own->second, "'" + name.as + "' is declared here");
        continue;
      }
      auto [it, inserted] = here.imports.try_emplace(name.as, Import{*target, name.asLoc});
      if (!inserted) {
        diags_.error(name.asLoc, "duplicate import of '" + name.as + "'");
        diags_.note(it->second.loc, "previously imported here");
      }
    }
  }
}

void Sema::bindImports() {
  ModuleState &here = state(module_);
  // The classes of node:fs, as types and for `instanceof`.
  for (const auto &entry : here.nodeNames) {
    const NodeName &n = entry.getValue();
    ClassDecl *cls = n.module != NodeModule::Fs ? nullptr
                     : n.name == "Stats"        ? statsClass_
                     : n.name == "Dirent"       ? direntClass_
                                                : nullptr;
    if (!cls)
      continue;
    typeScopes_[0].try_emplace(entry.getKey(), cls);
    scopes_[0].try_emplace(entry.getKey(), ScopeEntry{nullptr, nullptr, nullptr, cls});
  }
  for (const auto &entry : here.imports) {
    llvm::StringRef alias = entry.getKey();
    const Binding &target = entry.getValue().target;
    Module *source = target.module;
    // As a type.
    const llvm::StringMap<Stmt *> &types = source == module_ ? typeScopes_[0] : state(source).types;
    if (!typeScopes_[0].count(alias))
      if (auto found = types.find(target.name); found != types.end()) {
        Stmt *type = found->second;
        typeScopes_[0][alias] = type;
      }
    // As a value: a function or an enum, or a variable once the code of its
    // file has declared it.
    const llvm::StringMap<ScopeEntry> &values =
        source == module_ ? scopes_[0] : state(source).values;
    if (scopes_[0].count(alias))
      continue;
    auto found = values.find(target.name);
    if (found == values.end())
      continue;
    ScopeEntry value = found->second;
    if (!value.var && !value.func && !value.enumDecl && !value.cls)
      continue;
    scopes_[0][alias] = value;
    if (value.var && source != module_)
      here.importedVars.insert(value.var);
  }
}

void Sema::noteImportsInFunctions(SourceFile &program) {
  for (Module *module : program.modules)
    for (const auto &entry : state(module).imports)
      if (namesInFunctions_.contains(entry.getKey()))
        namesInFunctions_.insert(entry.getValue().target.name);
}

FunctionDecl *Sema::cycleCallee(FunctionDecl *fn, llvm::StringRef name) {
  Module *home = declModules_.lookup(fn);
  if (!home)
    return nullptr;
  const llvm::StringMap<ScopeEntry> &values = home == module_ ? scopes_[0] : state(home).values;
  auto found = values.find(name);
  if (found == values.end() || !found->second.func)
    return nullptr;
  return found->second.func->decl;
}

//===----------------------------------------------------------------------===//
// Top-level code
//===----------------------------------------------------------------------===//

void Sema::checkTopLevel(Module *module, llvm::StringRef until) {
  ModuleState &here = state(module);
  Module *outer = module_, *outerLoading = loading_;
  useModule(module);
  loading_ = module;
  here.busy = true;
  bindImports();
  if (here.flow) {
    flow_ = std::move(*here.flow);
    here.flow.reset();
  }
  std::vector<Stmt *> &items = module->file.items;
  while (here.next < items.size()) {
    Stmt *item = items[here.next++];
    if (isa<FunctionDecl>(item))
      continue;
    // A global may also go where the functions of the file take it.
    block_ = items;
    blockIndex_ = here.next - 1;
    checkStmt(item);
    auto *decl = dyn_cast<VarDeclStmt>(item);
    if (!until.empty() && decl &&
        llvm::any_of(decl->decls, [&](const VarDeclarator &d) { return d.name == until; }))
      break;
  }
  if (here.next == items.size())
    here.loaded = true;
  else
    here.flow = flow_; // for when the check goes on
  here.busy = false;
  loading_ = outerLoading;
  here.values = scopes_[0];
  here.types = typeScopes_[0];
  if (outer)
    useModule(outer);
}

Module *Sema::laterSource(llvm::StringRef name, llvm::StringRef &declared) {
  ModuleState &here = state(module_);
  declared = name;
  if (auto found = here.imports.find(name); found != here.imports.end()) {
    declared = found->second.target.name;
    Module *source = found->second.target.module;
    return state(source).loaded ? nullptr : source;
  }
  // A function of a file that loads later, checked early, uses a global of
  // its own file.
  if (loading_ && module_ != loading_ && !here.loaded && here.topLevelNames.contains(name))
    return module_;
  return nullptr;
}

Sema::ScopeEntry Sema::lookupValue(llvm::StringRef name) {
  ScopeEntry entry = lookup(name);
  if (entry.var || entry.func || entry.enumDecl || entry.cls || !loading_)
    return entry;
  llvm::StringRef declared;
  Module *source = laterSource(name, declared);
  // Read right as the file loads: an error however the types turn out (see
  // reportNotLoaded).
  if (!source || (functions_.empty() && module_ == loading_) || !loadEarly(source, declared))
    return entry;
  if (source == module_) {
    // Its own global, declared now; the scope of the file here is older.
    for (const auto &value : state(module_).values)
      scopes_[0].try_emplace(value.getKey(), value.getValue());
  } else {
    bindImports();
  }
  return lookup(name);
}

// The state of the check is saved as for a function body, and the file is
// checked as top-level code; its check goes on from there in its turn.
// Diagnostics are real even inside a trial: these statements are not
// checked again.
bool Sema::loadEarly(Module *source, llvm::StringRef name) {
  ModuleState &there = state(source);
  if (there.loaded || there.busy || !there.topLevelNames.contains(name))
    return false;
  BodyState saved = enterBody(nullptr);
  early_ = nullptr;
  llvm::SmallVector<llvm::StringRef, 2> initializing = std::move(initializing_);
  initializing_.clear();
  BinaryExpr *chain = std::exchange(concatChain_, nullptr);
  const BinaryExpr *mixed = std::exchange(mixedConcat_, nullptr);
  llvm::SmallPtrSet<const FuncSymbol *, 4> suspended = suspended_;
  suspended_.insert(inferring_.begin(), inferring_.end());
  bool muted = diags_.setMuted(false);

  checkTopLevel(source, name);

  diags_.setMuted(muted);
  suspended_ = std::move(suspended);
  concatChain_ = chain;
  mixedConcat_ = mixed;
  initializing_ = std::move(initializing);
  leaveBody(saved);
  return true;
}

//===----------------------------------------------------------------------===//
// Messages
//===----------------------------------------------------------------------===//

std::string Sema::loadChain(const Module *reader, const Module *source) {
  std::string chain;
  for (const Module *module : reader->loadPath)
    chain += (chain.empty() ? "" : " -> ") + module->name;
  if (source != reader)
    chain +=
        " -> (" + source->name +
        (llvm::is_contained(reader->loadPath, source) ? " already started)" : " not loaded yet)");
  return chain;
}

std::string Sema::readBeforeLoad(const Module *reader, const std::string &what,
                                 const Module *source, llvm::StringRef declared) {
  return reader->name + " reads " + what + " while it loads, but " + source->name +
         " is not loaded yet: " + loadChain(reader, source) +
         "; compute the value inside a function, or move '" + declared.str() +
         "' to a file outside the cycle";
}

bool Sema::reportNotLoaded(const NameExpr *name) {
  if (state(module_).badImports.contains(name->name))
    return true; // reported at the import
  llvm::StringRef declared;
  Module *source = laterSource(name->name, declared);
  if (!source)
    return false;
  std::string what = "'" + name->name.str() + "'";
  if (declared != name->name)
    what += " ('" + declared.str() + "' of " + source->name + ")";
  else if (source != loading_)
    what += " of " + source->name;
  Module *reader = loading_ ? loading_ : module_;
  if (functions_.empty() && module_ == loading_) {
    diags_.error(name->loc(), readBeforeLoad(reader, what, source, declared));
    return true;
  }
  // Its file could not be checked ahead of its turn: it is being checked,
  // and has not got to the declaration (see loadEarly).
  if (early_)
    diags_.error(name->loc(),
                 what + " has no type yet when " + reader->name + " first calls '" + early_->name +
                     "', which needs to know what '" + early_->name +
                     "' returns: the files import each other; declare the return type of '" +
                     early_->name + "'");
  else
    diags_.error(name->loc(),
                 what + " has no type yet where " + reader->name +
                     " creates this function: the files import each other; declare the "
                     "function with 'function' at the top level of the file, which is "
                     "checked after every file");
  return true;
}

bool Sema::reportImportAssign(const NameExpr *name, const VarSymbol *var) {
  if (!state(module_).importedVars.contains(var))
    return false;
  diags_.error(name->loc(), "cannot assign to '" + name->name +
                                "' because it is imported; change it in "
                                "the file that declares it, e.g. with a "
                                "function that file exports");
  return true;
}

//===----------------------------------------------------------------------===//
// Load order
//===----------------------------------------------------------------------===//

// What the code that runs at some point may read: of the globals it uses,
// directly or through the functions it calls, the one set last. Functions
// the code only creates run later, and do not count; neither do calls
// through function values, which are not known here.
void Sema::checkLoadOrder(SourceFile &program) {
  // When each global is set: the top-level declarations count up in the
  // order the program runs them.
  llvm::DenseMap<const VarSymbol *, unsigned> setAt;
  llvm::DenseMap<const VarSymbol *, Module *> homes;
  unsigned count = 0;
  for (Module *module : program.modules)
    for (Stmt *item : module->file.items)
      if (auto *decl = dyn_cast<VarDeclStmt>(item))
        for (const VarDeclarator &d : decl->decls)
          if (d.sym) {
            setAt[d.sym] = ++count;
            homes[d.sym] = module;
          }

  struct Use {
    const VarSymbol *var = nullptr;
    unsigned setAt = 0;
    llvm::SMLoc loc;
  };
  struct Effects {
    Use latest;
    std::vector<const FunctionDecl *> calls;
  };
  llvm::DenseMap<const FunctionDecl *, Effects> effects;
  std::vector<const FunctionDecl *> queue;
  auto need = [&](const FunctionDecl *fn) {
    if (effects.try_emplace(fn).second)
      queue.push_back(fn);
  };

  // The calls of top-level code, with the number of globals set by then.
  struct Site {
    const CallExpr *call;
    llvm::SmallVector<const FunctionDecl *, 2> called;
    unsigned time;
    Module *module;
  };
  std::vector<Site> sites;
  unsigned time = 0;
  for (Module *module : program.modules) {
    auto visit = [&](const Expr *e) {
      if (auto *name = dyn_cast<NameExpr>(e)) {
        // A global of a file that loads later, which Sema saw because that
        // file was checked out of turn (see loadEarly).
        auto found = name->var ? setAt.find(name->var) : setAt.end();
        if (found == setAt.end() || found->second <= time)
          return;
        Module *home = homes.lookup(name->var);
        std::string what = "'" + name->name.str() + "'";
        if (name->name != name->var->name)
          what += " ('" + name->var->name.str() + "' of " + home->name + ")";
        else
          what += " of " + home->name;
        if (home == module)
          diags_.error(name->loc(), "'" + name->name + "' is used before it is declared");
        else
          diags_.error(name->loc(), readBeforeLoad(module, what, home, name->var->name));
        return;
      }
      llvm::SmallVector<const FunctionDecl *, 2> constructed;
      auto *call = dyn_cast<CallExpr>(e);
      if (const CallExpr *ctor = constructorCall(e, constructed))
        call = const_cast<CallExpr *>(ctor);
      if (!call)
        return;
      Site site{call, constructed, time, module};
      if (constructed.empty())
        calledFunctions(call, site.called);
      if (site.called.empty())
        return;
      for (const FunctionDecl *fn : site.called)
        need(fn);
      sites.push_back(std::move(site));
    };
    for (Stmt *item : module->file.items) {
      if (isa<FunctionDecl>(item))
        continue;
      auto *decl = dyn_cast<VarDeclStmt>(item);
      if (!decl) {
        visitExprs(item, visit, /*intoFunctions=*/false);
        continue;
      }
      for (const VarDeclarator &d : decl->decls) {
        if (d.init)
          visitExprs(d.init, visit, /*intoFunctions=*/false);
        if (d.sym)
          ++time;
      }
    }
  }

  // What each function those calls reach does by itself.
  for (size_t i = 0; i < queue.size(); ++i) {
    const FunctionDecl *fn = queue[i];
    Effects own;
    auto visit = [&](const Expr *e) {
      if (auto *name = dyn_cast<NameExpr>(e)) {
        auto found = name->var ? setAt.find(name->var) : setAt.end();
        if (found != setAt.end() && found->second > own.latest.setAt)
          own.latest = {name->var, found->second, name->loc()};
      } else if (auto *call = dyn_cast<CallExpr>(e)) {
        llvm::SmallVector<const FunctionDecl *, 2> called;
        calledFunctions(call, called);
        own.calls.insert(own.calls.end(), called.begin(), called.end());
      } else {
        llvm::SmallVector<const FunctionDecl *, 2> called;
        constructorCall(e, called);
        own.calls.insert(own.calls.end(), called.begin(), called.end());
      }
    };
    for (const Param &param : fn->params)
      if (param.defaultValue)
        visitExprs(param.defaultValue, visit, /*intoFunctions=*/false);
    visitExprs(fn->body, visit, /*intoFunctions=*/false);
    for (const FunctionDecl *callee : own.calls)
      need(callee);
    effects[fn] = std::move(own);
  }
  // And with the functions they call, until nothing changes.
  for (bool changed = true; changed;) {
    changed = false;
    for (auto &entry : effects)
      for (const FunctionDecl *callee : entry.second.calls) {
        const Use &theirs = effects.find(callee)->second.latest;
        if (theirs.setAt > entry.second.latest.setAt) {
          entry.second.latest = theirs;
          changed = true;
        }
      }
  }

  for (const Site &site : sites) {
    for (const FunctionDecl *fn : site.called) {
      const Use &use = effects.find(fn)->second.latest;
      if (!use.var || use.setAt <= site.time)
        continue;
      Module *home = homes.lookup(use.var);
      std::string var = use.var->name.str();
      if (home == site.module)
        diags_.error(site.call->loc(),
                     calleeName(fn) + " uses '" + var +
                         "' before it is declared: JavaScript would throw a "
                         "ReferenceError when it runs here; move the declaration of '" +
                         var + "' up");
      else
        diags_.error(site.call->loc(),
                     site.module->name + " calls " + calleeName(fn) +
                         " while it loads, and it reads '" + var + "' of " + home->name +
                         ", which is not loaded yet: " + loadChain(site.module, home) +
                         "; make the call inside a function, or move '" + var +
                         "' to a file outside the cycle");
      diags_.note(use.loc, "'" + var + "' is used here");
      diags_.note(use.var->loc, "'" + var + "' is declared here");
      break; // one message for the call
    }
  }
}

//===----------------------------------------------------------------------===//
// Unused functions
//===----------------------------------------------------------------------===//

// The entry file is compiled whole; of the files it imports, only the
// functions the program reaches.
void Sema::findUnused(SourceFile &program) {
  if (program.modules.size() < 2)
    return;
  llvm::SmallPtrSet<const FunctionDecl *, 16> used;
  std::vector<const FunctionDecl *> queue;
  auto reach = [&](const Expr *e) {
    const FuncSymbol *fn = nullptr;
    if (auto *call = dyn_cast<CallExpr>(e))
      fn = call->func;
    else if (auto *name = dyn_cast<NameExpr>(e))
      fn = name->func;
    if (fn && fn->decl && used.insert(fn->decl).second)
      queue.push_back(fn->decl);
  };
  for (Module *module : program.modules)
    for (Stmt *item : module->file.items)
      if (module->isEntry || !isa<FunctionDecl>(item))
        visitExprs(item, reach);
  while (!queue.empty()) {
    const FunctionDecl *fn = queue.back();
    queue.pop_back();
    visitExprs(fn, reach);
  }
  for (Module *module : program.modules)
    if (!module->isEntry)
      for (Stmt *item : module->file.items)
        if (auto *fn = dyn_cast<FunctionDecl>(item); fn && !used.contains(fn))
          program.unusedFunctions.insert(fn);
}

} // namespace tz
