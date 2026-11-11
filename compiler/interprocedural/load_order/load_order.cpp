#include "compiler/interprocedural/load_order/load_order.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/module/load_chain/load_chain.h"
#include "compiler/program/module/module.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

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

  return ok(name);
}

std::vector<Diagnostic> LoadOrder::visitTopLevel(const std::vector<const Expr *> &exprs,
                                                 Module *module, int time,
                                                 LoadState &loadState) const {
  auto earlyReads = std::vector<Diagnostic>();
  for (const Expr *e : exprs) {
    const auto *name = dyn_cast<NameExpr>(e);
    if (name == nullptr) {
      this->rememberSite(e, module, time, loadState);
      continue;
    }
    auto read = LoadOrder::earlyRead(name, module, time, loadState);
    if (read.isErr()) {
      earlyReads.push_back({
          .kind = llvm::SourceMgr::DK_Error,
          .loc = name->loc(),
          .message = read.error().message,
      });
    }
  }

  return earlyReads;
}

void LoadOrder::rememberSite(const Expr *e, Module *module, int time, LoadState &loadState) const {
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

std::vector<Diagnostic> LoadOrder::topLevelUses(SourceFile &program, LoadState &loadState) const {
  auto earlyReads = std::vector<Diagnostic>();
  int time = 0;
  for (Module *module : program.modules) {
    for (Stmt *item : module->file.items) {
      if (isa<FunctionDecl>(item)) {
        continue;
      }
      auto *decl = dyn_cast<VarDeclStmt>(item);
      if (!decl) {
        auto exprs = AstWalker::exprsIn(item, false);
        auto found = this->visitTopLevel(exprs, module, time, loadState);
        llvm::append_range(earlyReads, found);
        continue;
      }
      for (const VarDeclarator &d : decl->decls) {
        if (d.init) {
          auto exprs = AstWalker::exprsIn(d.init, false);
          auto found = this->visitTopLevel(exprs, module, time, loadState);
          llvm::append_range(earlyReads, found);
        }
        if (d.sym) {
          ++time;
        }
      }
    }
  }

  return earlyReads;
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
      for (const Expr *e : AstWalker::exprsIn(param.defaultValue(), false)) {
        visit(e);
      }
    }
  }
  for (const Expr *e : AstWalker::exprsIn(fn->body, false)) {
    visit(e);
  }
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
    std::string chain = LoadChain::describe(site.module, home);
    std::string message =
        ErrorTextLateUse::otherFile(site.module->name, callee, use.var->name, home->name, chain);

    return err(LoadOrderLateUseError{
        .code = LoadOrderLateUseErrorCode::READ_BEFORE_LOADED,
        .message = message,
    });
  }

  return ok(fn);
}

std::vector<Diagnostic> LoadOrder::findLateUses(const LoadState &loadState) const {
  auto lateUses = std::vector<Diagnostic>();
  for (const Site &site : loadState.sites) {
    for (const FunctionDecl *fn : site.called) {
      const Use &use = loadState.effects.find(fn)->second.latest;
      ResultSetBeforeRun used = LoadOrder::lateUse(site, fn, use, loadState);
      if (used.isOk()) {
        continue;
      }
      std::string usedHere = ErrorTextLateUse::usedHere(use.var->name);
      std::string declared = ErrorTextLateUse::declared(use.var->name);
      lateUses.push_back({
          .kind = llvm::SourceMgr::DK_Error,
          .loc = site.call->loc(),
          .message = used.error().message,
      });
      lateUses.push_back({
          .kind = llvm::SourceMgr::DK_Note,
          .loc = use.loc,
          .message = usedHere,
      });
      lateUses.push_back({
          .kind = llvm::SourceMgr::DK_Note,
          .loc = use.var->loc,
          .message = declared,
      });
      break;
    }
  }

  return lateUses;
}

LoadOrder::ResultLoadOrderChecked LoadOrder::checkLoadOrder(SourceFile &program) const {
  LoadState loadState;
  LoadOrder::declarationOrder(program, loadState);
  auto earlyReads = this->topLevelUses(program, loadState);
  for (size_t i = 0; i < loadState.queue.size(); ++i) {
    const FunctionDecl *fn = loadState.queue[i];
    LoadOrder::functionEffects(fn, loadState);
  }
  LoadOrder::spreadEffects(loadState);
  auto lateUses = this->findLateUses(loadState);

  auto found = std::vector<Diagnostic>();
  llvm::append_range(found, earlyReads);
  llvm::append_range(found, lateUses);
  if (!found.empty()) {
    return err(found);
  }

  return ok();
}

}
