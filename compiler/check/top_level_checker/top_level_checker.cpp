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
    bool started = llvm::is_contained(reader->loadPath, source);
    chain += ErrorTextNotLoaded::pending(source->name, started);
  }

  return chain;
}

std::string TopLevelChecker::loadLabel(llvm::StringRef name, llvm::StringRef declared,
                                       const Module *source) const {
  if (declared != name) {
    return ErrorTextNotLoaded::label(name, declared, source->name);
  }
  if (source != this->checker.loading_) {
    return ErrorTextNotLoaded::labelOf(name, source->name);
  }

  return ErrorTextNotLoaded::quoted(name);
}

TopLevelChecker::ResultSourceIsLoaded TopLevelChecker::checkLoaded(const NameExpr *name) {
  if (this->checker.state(this->checker.module_).badImports.contains(name->name)) {
    return err(TopLevelCheckerCheckLoadedError{
        .code = TopLevelCheckerCheckLoadedErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }

  llvm::StringRef declared;
  Module *source = this->laterSource(name->name, declared);
  Module *reader = this->checker.loading_ ? this->checker.loading_ : this->checker.module_;
  std::string what = source ? this->loadLabel(name->name, declared, source) : "";
  bool topLevel =
      this->checker.functions_.empty() && this->checker.module_ == this->checker.loading_;
  if (source && topLevel) {
    std::string chain = TopLevelChecker::loadChain(reader, source);
    std::string message =
        ErrorTextNotLoaded::whileLoads(reader->name, what, source->name, chain, declared);

    return err(TopLevelCheckerCheckLoadedError{
        .code = TopLevelCheckerCheckLoadedErrorCode::READ_WHILE_LOADING,
        .message = message,
    });
  }
  if (source && this->checker.early_) {
    std::string message =
        ErrorTextNotLoaded::beforeCall(what, reader->name, this->checker.early_->name);

    return err(TopLevelCheckerCheckLoadedError{
        .code = TopLevelCheckerCheckLoadedErrorCode::NO_TYPE_BEFORE_CALL,
        .message = message,
    });
  }
  if (source) {
    std::string message = ErrorTextNotLoaded::inFunction(what, reader->name);

    return err(TopLevelCheckerCheckLoadedError{
        .code = TopLevelCheckerCheckLoadedErrorCode::NO_TYPE_IN_FUNCTION,
        .message = message,
    });
  }

  return ok(name);
}

TopLevelChecker::ResultVarNotImported
TopLevelChecker::checkImportAssign(const NameExpr *name, const VarSymbol *var) const {
  if (this->checker.state(this->checker.module_).importedVars.contains(var)) {
    std::string message = ErrorTextImportAssign::imported(name->name);

    return err(TopLevelCheckerCheckImportAssignError{
        .code = TopLevelCheckerCheckImportAssignErrorCode::IMPORTED_VARIABLE,
        .message = message,
    });
  }

  return ok(var);
}

}
