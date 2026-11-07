#include "compiler/pipeline/check_program/whole_program/load_order/load_order.h"
#include "compiler/pipeline/check_program/ast_walker/ast_walker.h"
#include "compiler/pipeline/check_program/check/top_level_checker/top_level_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

LoadOrder::LoadOrder(CheckState &owner)
    : state(owner) {}

const FunctionDecl *LoadOrder::directFunction(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    expr = conv->operand;
  }
  auto *function = dyn_cast<FunctionExpr>(expr);
  if (function) {
    return function->fn;
  }
  auto *name = dyn_cast<NameExpr>(expr);
  if (name && name->func()) {
    return name->func()->decl;
  }
  const VarSymbol *var = name ? name->var() : nullptr;
  if (var && var->function()) {
    return var->function();
  }
  if (var && var->isConst) {
    return var->lambda();
  }

  return nullptr;
}

void LoadOrder::calledFunctions(const CallExpr *call,
                                llvm::SmallVectorImpl<const FunctionDecl *> &called) {
  if (call->func() && call->func()->decl) {
    called.push_back(call->func()->decl);
  } else if (call->method()) {
    called.push_back(call->method()->decl);
  }
  for (const UnionMethod &m : call->unionMethods()) {
    const FunctionDecl *direct =
        !m.method && call->callee ? LoadOrder::directFunction(call->callee) : nullptr;
    if (m.method) {
      called.push_back(m.method->decl);
    } else if (direct) {
      called.push_back(direct);
    }
  }
  bool arrayCallback =
      call->builtin() >= Builtin::ArrForEach && call->builtin() <= Builtin::ArrSortWith;
  if (arrayCallback || BuiltinGroups::isResultCallback(call->builtin())) {
    for (const Expr *arg : call->args) {
      const FunctionDecl *fn = LoadOrder::directFunction(arg);
      if (fn) {
        called.push_back(fn);
      }
    }
  }
}

const CallExpr *LoadOrder::constructorCall(const Expr *expr,
                                           llvm::SmallVectorImpl<const FunctionDecl *> &called) {
  auto *create = dyn_cast<NewExpr>(expr);
  if (!create || !create->cls() || !create->ctor()) {
    return nullptr;
  }
  if (create->cls()->init) {
    called.push_back(create->cls()->init);
  }
  if (create->ctor()->func()) {
    called.push_back(create->ctor()->func()->decl);
  }

  return called.empty() ? nullptr : create->ctor();
}

std::string LoadOrder::calleeName(const FunctionDecl *fn) {
  const ClassDecl *cls = fn->cls();
  if (!cls) {
    return ErrorTextLateUse::function(fn->name);
  }
  if (fn == cls->init || fn->isConstructor()) {
    return ErrorTextLateUse::creation(cls->name);
  }

  return ErrorTextLateUse::method(cls->name, fn->name);
}

void LoadOrder::need(const FunctionDecl *fn, LoadState &loadState) {
  bool added = loadState.effects.try_emplace(fn).second;
  if (added) {
    loadState.queue.push_back(fn);
  }
}

void LoadOrder::declarationOrder(SourceFile &program, LoadState &loadState) {
  int count = 0;
  for (Module *module : program.modules) {
    for (Stmt *item : module->file.items) {
      auto *decl = dyn_cast<VarDeclStmt>(item);
      if (!decl) {
        continue;
      }
      for (const VarDeclarator &d : decl->decls) {
        if (d.sym) {
          ++count;
          loadState.setAt[d.sym] = count;
          loadState.homes[d.sym] = module;
        }
      }
    }
  }
}

LoadOrder::ResultReadAfterSet LoadOrder::earlyRead(const NameExpr *name, const Module *module,
                                                   int time, const LoadState &loadState) {
  const VarSymbol *var = name->var();
  auto found = var ? loadState.setAt.find(var) : loadState.setAt.end();
  bool early = found != loadState.setAt.end() && found->second > time;
  Module *home = early ? loadState.homes.lookup(var) : nullptr;
  if (early && home == module) {
    std::string message = ErrorTextEarlyRead::sameFile(name->name);

    return err(LoadOrderEarlyReadError{
        .code = LoadOrderEarlyReadErrorCode::USED_BEFORE_DECLARED,
        .message = message,
    });
  }
  if (early) {
    std::string what = ErrorTextEarlyRead::label(name->name, var->name, home->name);
    std::string chain = TopLevelChecker::loadChain(module, home);
    std::string message =
        ErrorTextNotLoaded::whileLoads(module->name, what, home->name, chain, var->name);

    return err(LoadOrderEarlyReadError{
        .code = LoadOrderEarlyReadErrorCode::READ_BEFORE_LOADED,
        .message = message,
    });
  }

  return ok(name);
}

void LoadOrder::visitTopLevel(const Expr *e, Module *module, int time, LoadState &loadState) {
  auto *name = dyn_cast<NameExpr>(e);
  if (name) {
    ResultReadAfterSet read = LoadOrder::earlyRead(name, module, time, loadState);
    if (read.isErr()) {
      this->state.getDiagnostics().error(name->loc(), read.error().message);
    }
    return;
  }

  llvm::SmallVector<const FunctionDecl *, 2> constructed;
  const CallExpr *ctor = LoadOrder::constructorCall(e, constructed);
  const CallExpr *call = ctor ? ctor : dyn_cast<CallExpr>(e);
  if (!call) {
    return;
  }
  Site site{.time = time, .module = module, .call = call, .called = constructed};
  if (constructed.empty()) {
    LoadOrder::calledFunctions(call, site.called);
  }
  if (site.called.empty()) {
    return;
  }
  for (const FunctionDecl *fn : site.called) {
    LoadOrder::need(fn, loadState);
  }
  loadState.sites.push_back(std::move(site));
}

void LoadOrder::topLevelUses(SourceFile &program, LoadState &loadState) {
  int time = 0;
  for (Module *module : program.modules) {
    auto visit = [&](const Expr *e) { this->visitTopLevel(e, module, time, loadState); };
    for (Stmt *item : module->file.items) {
      if (isa<FunctionDecl>(item)) {
        continue;
      }
      auto *decl = dyn_cast<VarDeclStmt>(item);
      if (!decl) {
        AstWalker::visitExprs(item, visit, false);
        continue;
      }
      for (const VarDeclarator &d : decl->decls) {
        if (d.init) {
          AstWalker::visitExprs(d.init, visit, false);
        }
        if (d.sym) {
          ++time;
        }
      }
    }
  }
}

void LoadOrder::functionEffects(const FunctionDecl *fn, LoadState &loadState) {
  Effects own;
  auto visit = [&](const Expr *e) {
    auto *name = dyn_cast<NameExpr>(e);
    auto *call = dyn_cast<CallExpr>(e);
    if (name) {
      auto found = name->var() ? loadState.setAt.find(name->var()) : loadState.setAt.end();
      bool later = found != loadState.setAt.end() && found->second > own.latest.setAt;
      if (later) {
        own.latest = Use{.setAt = found->second, .loc = name->loc(), .var = name->var()};
      }
      return;
    }
    llvm::SmallVector<const FunctionDecl *, 2> called;
    if (call) {
      LoadOrder::calledFunctions(call, called);
    } else {
      LoadOrder::constructorCall(e, called);
    }
    own.calls.insert(own.calls.end(), called.begin(), called.end());
  };
  for (const Param &param : fn->params) {
    if (param.defaultValue()) {
      AstWalker::visitExprs(param.defaultValue(), visit, false);
    }
  }
  AstWalker::visitExprs(fn->body, visit, false);
  for (const FunctionDecl *callee : own.calls) {
    LoadOrder::need(callee, loadState);
  }
  loadState.effects[fn] = std::move(own);
}

void LoadOrder::spreadEffects(LoadState &loadState) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto &entry : loadState.effects) {
      for (const FunctionDecl *callee : entry.second.calls) {
        const Use &theirs = loadState.effects.find(callee)->second.latest;
        if (theirs.setAt > entry.second.latest.setAt) {
          entry.second.latest = theirs;
          changed = true;
        }
      }
    }
  }
}

LoadOrder::ResultSetBeforeRun LoadOrder::lateUse(const Site &site, const FunctionDecl *fn,
                                                 const Use &use, const LoadState &loadState) {
  bool late = use.var && use.setAt > site.time;
  Module *home = late ? loadState.homes.lookup(use.var) : nullptr;
  if (late && home == site.module) {
    std::string callee = LoadOrder::calleeName(fn);
    std::string message = ErrorTextLateUse::sameFile(callee, use.var->name);

    return err(LoadOrderLateUseError{
        .code = LoadOrderLateUseErrorCode::USED_BEFORE_DECLARED,
        .message = message,
    });
  }
  if (late) {
    std::string callee = LoadOrder::calleeName(fn);
    std::string chain = TopLevelChecker::loadChain(site.module, home);
    std::string message =
        ErrorTextLateUse::otherFile(site.module->name, callee, use.var->name, home->name, chain);

    return err(LoadOrderLateUseError{
        .code = LoadOrderLateUseErrorCode::READ_BEFORE_LOADED,
        .message = message,
    });
  }

  return ok(fn);
}

void LoadOrder::reportSites(const LoadState &loadState) {
  for (const Site &site : loadState.sites) {
    for (const FunctionDecl *fn : site.called) {
      const Use &use = loadState.effects.find(fn)->second.latest;
      ResultSetBeforeRun used = LoadOrder::lateUse(site, fn, use, loadState);
      if (used.isOk()) {
        continue;
      }
      std::string usedHere = ErrorTextLateUse::usedHere(use.var->name);
      std::string declared = ErrorTextLateUse::declared(use.var->name);
      this->state.getDiagnostics().error(site.call->loc(), used.error().message);
      this->state.getDiagnostics().note(use.loc, usedHere);
      this->state.getDiagnostics().note(use.var->loc, declared);
      break;
    }
  }
}

void LoadOrder::checkLoadOrder(SourceFile &program) {
  LoadState loadState;
  LoadOrder::declarationOrder(program, loadState);
  this->topLevelUses(program, loadState);
  for (size_t i = 0; i < loadState.queue.size(); ++i) {
    const FunctionDecl *fn = loadState.queue[i];
    LoadOrder::functionEffects(fn, loadState);
  }
  LoadOrder::spreadEffects(loadState);
  this->reportSites(loadState);
}

}
