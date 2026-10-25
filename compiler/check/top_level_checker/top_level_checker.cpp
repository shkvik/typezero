#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/declare/import_binder/import_binder.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

TopLevelChecker::TopLevelChecker(Sema &sema) : sema_(sema) {}

void TopLevelChecker::checkTopLevel(Module *module, llvm::StringRef until) {
  Sema::ModuleState &here = sema_.state(module);
  Module *outer = sema_.module_, *outerLoading = sema_.loading_;
  sema_.moduleDeclarer().useModule(module);
  sema_.loading_ = module;
  here.busy = true;
  sema_.importBinder().bindImports();
  if (here.flow) {
    sema_.flow_ = std::move(*here.flow);
    here.flow.reset();
  }
  std::vector<Stmt *> &items = module->file.items;
  while (here.next < items.size()) {
    Stmt *item = items[here.next++];
    if (isa<FunctionDecl>(item)) {
      continue;
    }
    // A global may also go where the functions of the file take it.
    sema_.block_ = items;
    sema_.blockIndex_ = here.next - 1;
    sema_.stmtChecker().checkStmt(item);
    auto *decl = dyn_cast<VarDeclStmt>(item);
    if (!until.empty() && decl &&
        llvm::any_of(decl->decls, [&](const VarDeclarator &d) { return d.name == until; })) {
      break;
    }
  }
  if (here.next == items.size()) {
    here.loaded = true;
  } else {
    here.flow = sema_.flow_; // for when the check goes on
  }
  here.busy = false;
  sema_.loading_ = outerLoading;
  here.values = sema_.scopes_[0];
  here.types = sema_.typeScopes_[0];
  if (outer) {
    sema_.moduleDeclarer().useModule(outer);
  }
}

Module *TopLevelChecker::laterSource(llvm::StringRef name, llvm::StringRef &declared) {
  Sema::ModuleState &here = sema_.state(sema_.module_);
  declared = name;
  if (auto found = here.imports.find(name); found != here.imports.end()) {
    declared = found->second.target.name;
    Module *source = found->second.target.module;
    return sema_.state(source).loaded ? nullptr : source;
  }
  // A function of a file that loads later, checked early, uses a global of
  // its own file.
  if (sema_.loading_ && sema_.module_ != sema_.loading_ && !here.loaded && here.topLevelNames.contains(name)) {
    return sema_.module_;
  }

  return nullptr;
}

Sema::ScopeEntry TopLevelChecker::lookupValue(llvm::StringRef name) {
  Sema::ScopeEntry entry = sema_.scopes().lookup(name);
  if (entry.var || entry.func || entry.enumDecl || entry.cls || !sema_.loading_) {
    return entry;
  }
  llvm::StringRef declared;
  Module *source = laterSource(name, declared);
  // Read right as the file loads: an error however the types turn out (see
  // reportNotLoaded).
  if (!source || (sema_.functions_.empty() && sema_.module_ == sema_.loading_) || !loadEarly(source, declared)) {
    return entry;
  }
  if (source == sema_.module_) {
    // Its own global, declared now; the scope of the file here is older.
    for (const auto &value : sema_.state(sema_.module_).values) {
      sema_.scopes_[0].try_emplace(value.getKey(), value.getValue());
    }
  } else {
    sema_.importBinder().bindImports();
  }

  return sema_.scopes().lookup(name);
}

// The state of the check is saved as for a function body, and the file is
// checked as top-level code; its check goes on from there in its turn.
// Diagnostics are real even inside a trial: these statements are not
// checked again.
bool TopLevelChecker::loadEarly(Module *source, llvm::StringRef name) {
  Sema::ModuleState &there = sema_.state(source);
  if (there.loaded || there.busy || !there.topLevelNames.contains(name)) {
    return false;
  }
  Sema::BodyState saved = sema_.returnInferrer().enterBody(nullptr);
  sema_.early_ = nullptr;
  llvm::SmallVector<llvm::StringRef, 2> initializing = std::move(sema_.initializing_);
  sema_.initializing_.clear();
  BinaryExpr *chain = std::exchange(sema_.concatChain_, nullptr);
  const BinaryExpr *mixed = std::exchange(sema_.mixedConcat_, nullptr);
  llvm::SmallPtrSet<const FuncSymbol *, 4> suspended = sema_.suspended_;
  sema_.suspended_.insert(sema_.inferring_.begin(), sema_.inferring_.end());
  bool muted = sema_.diags_.setMuted(false);

  checkTopLevel(source, name);

  sema_.diags_.setMuted(muted);
  sema_.suspended_ = std::move(suspended);
  sema_.concatChain_ = chain;
  sema_.mixedConcat_ = mixed;
  sema_.initializing_ = std::move(initializing);
  sema_.returnInferrer().leaveBody(saved);

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
  if (sema_.state(sema_.module_).badImports.contains(name->name)) {
    return true; // reported at the import
  }
  llvm::StringRef declared;
  Module *source = laterSource(name->name, declared);
  if (!source) {
    return false;
  }
  std::string what = "'" + name->name.str() + "'";
  if (declared != name->name) {
    what += " ('" + declared.str() + "' of " + source->name + ")";
  } else if (source != sema_.loading_) {
    what += " of " + source->name;
  }
  Module *reader = sema_.loading_ ? sema_.loading_ : sema_.module_;
  if (sema_.functions_.empty() && sema_.module_ == sema_.loading_) {
    sema_.diags_.error(name->loc(), readBeforeLoad(reader, what, source, declared));
    return true;
  }
  // Its file could not be checked ahead of its turn: it is being checked,
  // and has not got to the declaration (see loadEarly).
  if (sema_.early_) {
    sema_.diags_.error(name->loc(),
                 what + " has no type yet when " + reader->name + " first calls '" + sema_.early_->name +
                     "', which needs to know what '" + sema_.early_->name +
                     "' returns: the files import each other; declare the return type of '" +
                     sema_.early_->name + "'");
  } else {
    sema_.diags_.error(name->loc(),
                 what + " has no type yet where " + reader->name +
                     " creates this function: the files import each other; declare the "
                     "function with 'function' at the top level of the file, which is "
                     "checked after every file");
  }

  return true;
}

bool TopLevelChecker::reportImportAssign(const NameExpr *name, const VarSymbol *var) {
  if (!sema_.state(sema_.module_).importedVars.contains(var)) {
    return false;
  }
  sema_.diags_.error(name->loc(), "cannot assign to '" + name->name +
                                "' because it is imported; change it in "
                                "the file that declares it, e.g. with a "
                                "function that file exports");

  return true;
}

} // namespace tz
