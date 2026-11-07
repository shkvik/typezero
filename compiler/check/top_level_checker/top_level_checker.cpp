#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/declare/import_binder/import_binder.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

TopLevelChecker::TopLevelChecker(Checker &owner)
    : checker(owner) {}

void TopLevelChecker::checkTopLevel(Module *module, llvm::StringRef until) {
  Checker::ModuleState &here = this->checker.state(module);
  Module *outer = this->checker.module_;
  Module *outerLoading = this->checker.loading_;
  this->checker.moduleDeclarer().useModule(module);
  this->checker.loading_ = module;
  here.busy = true;
  this->checker.importBinder().bindImports();
  if (here.flow) {
    this->checker.flow_ = std::move(*here.flow);
    here.flow.reset();
  }
  std::vector<Stmt *> &items = module->file.items;
  while (here.next < items.size()) {
    Stmt *item = items[here.next++];
    if (isa<FunctionDecl>(item)) {
      continue;
    }
    this->checker.block_ = items;
    this->checker.blockIndex_ = here.next - 1;
    this->checker.stmtChecker().checkStmt(item);
    auto *decl = dyn_cast<VarDeclStmt>(item);
    if (!until.empty() && decl &&
        llvm::any_of(decl->decls, [&](const VarDeclarator &d) { return d.name == until; })) {
      break;
    }
  }
  if (here.next == items.size()) {
    here.loaded = true;
    this->checker.emitterChecker().reportUnlistenedErrors();
  } else {
    here.flow = this->checker.flow_;
  }
  here.busy = false;
  this->checker.loading_ = outerLoading;
  here.values = this->checker.scopes_[0];
  here.types = this->checker.typeScopes_[0];
  if (outer) {
    this->checker.moduleDeclarer().useModule(outer);
  }
}

Module *TopLevelChecker::laterSource(llvm::StringRef name, llvm::StringRef &declared) {
  Checker::ModuleState &here = this->checker.state(this->checker.module_);
  declared = name;
  if (auto found = here.imports.find(name); found != here.imports.end()) {
    declared = found->second.target.name;
    Module *source = found->second.target.module;
    return this->checker.state(source).loaded ? nullptr : source;
  }
  if (this->checker.loading_ && this->checker.module_ != this->checker.loading_ && !here.loaded &&
      here.topLevelNames.contains(name)) {
    return this->checker.module_;
  }

  return nullptr;
}

ScopeEntry TopLevelChecker::lookupValue(llvm::StringRef name) {
  ScopeEntry entry = this->checker.scopes().lookup(name);
  if (entry.found() || !this->checker.loading_) {
    return entry;
  }
  llvm::StringRef declared;
  Module *source = this->laterSource(name, declared);
  if (!source ||
      (this->checker.functions_.empty() && this->checker.module_ == this->checker.loading_) ||
      !this->loadEarly(source, declared)) {
    return entry;
  }
  if (source == this->checker.module_) {
    for (const auto &value : this->checker.state(this->checker.module_).values) {
      this->checker.scopes_[0].try_emplace(value.getKey(), value.getValue());
    }
  } else {
    this->checker.importBinder().bindImports();
  }

  return this->checker.scopes().lookup(name);
}

bool TopLevelChecker::loadEarly(Module *source, llvm::StringRef name) {
  Checker::ModuleState &there = this->checker.state(source);
  if (there.loaded || there.busy || !there.topLevelNames.contains(name)) {
    return false;
  }
  Checker::BodyState saved = this->checker.returnInferrer().enterBody(nullptr);
  this->checker.early_ = nullptr;
  llvm::SmallVector<llvm::StringRef, 2> initializing = std::move(this->checker.initializing_);
  this->checker.initializing_.clear();
  BinaryExpr *chain = std::exchange(this->checker.concatChain_, nullptr);
  const BinaryExpr *mixed = std::exchange(this->checker.mixedConcat_, nullptr);
  llvm::SmallPtrSet<const FuncSymbol *, 4> suspended = this->checker.suspended_;
  this->checker.suspended_.insert(this->checker.inferring_.begin(), this->checker.inferring_.end());
  bool muted = this->checker.diags_.setMuted(false);

  this->checkTopLevel(source, name);

  this->checker.diags_.setMuted(muted);
  this->checker.suspended_ = std::move(suspended);
  this->checker.concatChain_ = chain;
  this->checker.mixedConcat_ = mixed;
  this->checker.initializing_ = std::move(initializing);
  this->checker.returnInferrer().leaveBody(saved);

  return true;
}

std::string TopLevelChecker::loadChain(const Module *reader, const Module *source) {
  std::string chain;
  for (const Module *module : reader->loadPath) {
    chain += (chain.empty() ? "" : " -> ") + module->name;
  }
  if (source != reader) {
    chain +=
        " -> (" + source->name +
        (llvm::is_contained(reader->loadPath, source) ? " already started)" : " not loaded yet)");
  }

  return chain;
}

std::string TopLevelChecker::readBeforeLoad(const Module *reader, const std::string &what,
                                            const Module *source, llvm::StringRef declared) {
  return reader->name + " reads " + what + " while it loads, but " + source->name +
         " is not loaded yet: " + TopLevelChecker::loadChain(reader, source) +
         "; compute the value inside a function, or move '" + declared.str() +
         "' to a file outside the cycle";
}

bool TopLevelChecker::reportNotLoaded(const NameExpr *name) {
  if (this->checker.state(this->checker.module_).badImports.contains(name->name)) {
    return true;
  }
  llvm::StringRef declared;
  Module *source = this->laterSource(name->name, declared);
  if (!source) {
    return false;
  }
  std::string what = "'" + name->name.str() + "'";
  if (declared != name->name) {
    what += " ('" + declared.str() + "' of " + source->name + ")";
  } else if (source != this->checker.loading_) {
    what += " of " + source->name;
  }
  Module *reader = this->checker.loading_ ? this->checker.loading_ : this->checker.module_;
  if (this->checker.functions_.empty() && this->checker.module_ == this->checker.loading_) {
    this->checker.diags_.error(name->loc(),
                               TopLevelChecker::readBeforeLoad(reader, what, source, declared));
    return true;
  }
  if (this->checker.early_) {
    this->checker.diags_.error(
        name->loc(), what + " has no type yet when " + reader->name + " first calls '" +
                         this->checker.early_->name + "', which needs to know what '" +
                         this->checker.early_->name +
                         "' returns: the files import each other; declare the return type of '" +
                         this->checker.early_->name + "'");
  } else {
    this->checker.diags_.error(
        name->loc(), what + " has no type yet where " + reader->name +
                         " creates this function: the files import each other; declare the "
                         "function with 'function' at the top level of the file, which is "
                         "checked after every file");
  }

  return true;
}

bool TopLevelChecker::reportImportAssign(const NameExpr *name, const VarSymbol *var) {
  if (!this->checker.state(this->checker.module_).importedVars.contains(var)) {
    return false;
  }
  this->checker.diags_.error(name->loc(), "cannot assign to '" + name->name +
                                              "' because it is imported; change it in "
                                              "the file that declares it, e.g. with a "
                                              "function that file exports");

  return true;
}

}
