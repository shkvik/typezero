#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/declare/import_binder/import_binder.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

TopLevelChecker::TopLevelChecker(Checker &checker) : checker_(checker) {}

void TopLevelChecker::checkTopLevel(Module *module, llvm::StringRef until) {
  Checker::ModuleState &here = checker_.state(module);
  Module *outer = checker_.module_, *outerLoading = checker_.loading_;
  checker_.moduleDeclarer().useModule(module);
  checker_.loading_ = module;
  here.busy = true;
  checker_.importBinder().bindImports();
  if (here.flow) {
    checker_.flow_ = std::move(*here.flow);
    here.flow.reset();
  }
  std::vector<Stmt *> &items = module->file.items;
  while (here.next < items.size()) {
    Stmt *item = items[here.next++];
    if (isa<FunctionDecl>(item)) {
      continue;
    }
    checker_.block_ = items;
    checker_.blockIndex_ = here.next - 1;
    checker_.stmtChecker().checkStmt(item);
    auto *decl = dyn_cast<VarDeclStmt>(item);
    if (!until.empty() && decl &&
        llvm::any_of(decl->decls, [&](const VarDeclarator &d) { return d.name == until; })) {
      break;
    }
  }
  if (here.next == items.size()) {
    here.loaded = true;
    checker_.emitterChecker().reportUnlistenedErrors();
  } else {
    here.flow = checker_.flow_;
  }
  here.busy = false;
  checker_.loading_ = outerLoading;
  here.values = checker_.scopes_[0];
  here.types = checker_.typeScopes_[0];
  if (outer) {
    checker_.moduleDeclarer().useModule(outer);
  }
}

Module *TopLevelChecker::laterSource(llvm::StringRef name, llvm::StringRef &declared) {
  Checker::ModuleState &here = checker_.state(checker_.module_);
  declared = name;
  if (auto found = here.imports.find(name); found != here.imports.end()) {
    declared = found->second.target.name;
    Module *source = found->second.target.module;
    return checker_.state(source).loaded ? nullptr : source;
  }
  if (checker_.loading_ && checker_.module_ != checker_.loading_ && !here.loaded &&
      here.topLevelNames.contains(name)) {
    return checker_.module_;
  }

  return nullptr;
}

Checker::ScopeEntry TopLevelChecker::lookupValue(llvm::StringRef name) {
  Checker::ScopeEntry entry = checker_.scopes().lookup(name);
  if (entry.var || entry.func || entry.enumDecl || entry.cls || !checker_.loading_) {
    return entry;
  }
  llvm::StringRef declared;
  Module *source = laterSource(name, declared);
  if (!source || (checker_.functions_.empty() && checker_.module_ == checker_.loading_) ||
      !loadEarly(source, declared)) {
    return entry;
  }
  if (source == checker_.module_) {
    for (const auto &value : checker_.state(checker_.module_).values) {
      checker_.scopes_[0].try_emplace(value.getKey(), value.getValue());
    }
  } else {
    checker_.importBinder().bindImports();
  }

  return checker_.scopes().lookup(name);
}

bool TopLevelChecker::loadEarly(Module *source, llvm::StringRef name) {
  Checker::ModuleState &there = checker_.state(source);
  if (there.loaded || there.busy || !there.topLevelNames.contains(name)) {
    return false;
  }
  Checker::BodyState saved = checker_.returnInferrer().enterBody(nullptr);
  checker_.early_ = nullptr;
  llvm::SmallVector<llvm::StringRef, 2> initializing = std::move(checker_.initializing_);
  checker_.initializing_.clear();
  BinaryExpr *chain = std::exchange(checker_.concatChain_, nullptr);
  const BinaryExpr *mixed = std::exchange(checker_.mixedConcat_, nullptr);
  llvm::SmallPtrSet<const FuncSymbol *, 4> suspended = checker_.suspended_;
  checker_.suspended_.insert(checker_.inferring_.begin(), checker_.inferring_.end());
  bool muted = checker_.diags_.setMuted(false);

  checkTopLevel(source, name);

  checker_.diags_.setMuted(muted);
  checker_.suspended_ = std::move(suspended);
  checker_.concatChain_ = chain;
  checker_.mixedConcat_ = mixed;
  checker_.initializing_ = std::move(initializing);
  checker_.returnInferrer().leaveBody(saved);

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
         " is not loaded yet: " + loadChain(reader, source) +
         "; compute the value inside a function, or move '" + declared.str() +
         "' to a file outside the cycle";
}

bool TopLevelChecker::reportNotLoaded(const NameExpr *name) {
  if (checker_.state(checker_.module_).badImports.contains(name->name)) {
    return true;
  }
  llvm::StringRef declared;
  Module *source = laterSource(name->name, declared);
  if (!source) {
    return false;
  }
  std::string what = "'" + name->name.str() + "'";
  if (declared != name->name) {
    what += " ('" + declared.str() + "' of " + source->name + ")";
  } else if (source != checker_.loading_) {
    what += " of " + source->name;
  }
  Module *reader = checker_.loading_ ? checker_.loading_ : checker_.module_;
  if (checker_.functions_.empty() && checker_.module_ == checker_.loading_) {
    checker_.diags_.error(name->loc(), readBeforeLoad(reader, what, source, declared));
    return true;
  }
  if (checker_.early_) {
    checker_.diags_.error(
        name->loc(), what + " has no type yet when " + reader->name + " first calls '" +
                         checker_.early_->name + "', which needs to know what '" +
                         checker_.early_->name +
                         "' returns: the files import each other; declare the return type of '" +
                         checker_.early_->name + "'");
  } else {
    checker_.diags_.error(name->loc(),
                          what + " has no type yet where " + reader->name +
                              " creates this function: the files import each other; declare the "
                              "function with 'function' at the top level of the file, which is "
                              "checked after every file");
  }

  return true;
}

bool TopLevelChecker::reportImportAssign(const NameExpr *name, const VarSymbol *var) {
  if (!checker_.state(checker_.module_).importedVars.contains(var)) {
    return false;
  }
  checker_.diags_.error(name->loc(), "cannot assign to '" + name->name +
                                         "' because it is imported; change it in "
                                         "the file that declares it, e.g. with a "
                                         "function that file exports");

  return true;
}

}
