#include "compiler/whole_program/load_order/load_order.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// The function a callee or a callback names directly, if any.
const FunctionDecl *directFunction(const Expr *expr) {
  while (auto *cast = dyn_cast<CastExpr>(expr)) {
    expr = cast->operand;
  }
  if (auto *fe = dyn_cast<FunctionExpr>(expr)) {
    return fe->fn;
  }
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    if (name->func) {
      return name->func->decl;
    }
    if (const VarSymbol *var = name->var) {
      return var->function ? var->function : var->isConst ? var->lambda : nullptr;
    }
  }

  return nullptr;
}

// The functions a call runs before it returns: the function it calls, and
// the callbacks a built-in such as map or forEach calls.
void calledFunctions(const CallExpr *call, llvm::SmallVectorImpl<const FunctionDecl *> &called) {
  if (call->func && call->func->decl) {
    called.push_back(call->func->decl);
  } else if (call->method) {
    called.push_back(call->method->decl);
  }
  for (const CallExpr::UnionMethod &m : call->unionMethods) {
    if (m.method) {
      called.push_back(m.method->decl);
    } else if (const FunctionDecl *fn = call->callee ? directFunction(call->callee) : nullptr) {
      called.push_back(fn); // `(() => ...)()`
    }
  }
  if ((call->builtin >= Builtin::ArrForEach && call->builtin <= Builtin::ArrSortWith) ||
      isResultCallback(call->builtin) || call->builtin == Builtin::SafeTry) {
    for (const Expr *arg : call->args) {
      if (const FunctionDecl *fn = directFunction(arg)) {
        called.push_back(fn);
      }
    }
  }
}

// What `new C()` runs: the initializers of the fields and the constructor.
// The call of the constructor stands for it.
const CallExpr *constructorCall(const Expr *expr,
                                llvm::SmallVectorImpl<const FunctionDecl *> &called) {
  auto *create = dyn_cast<NewExpr>(expr);
  if (!create || !create->cls || !create->ctor) {
    return nullptr;
  }
  if (create->cls->init) {
    called.push_back(create->cls->init);
  }
  if (create->ctor->func) {
    called.push_back(create->ctor->func->decl);
  }

  return called.empty() ? nullptr : create->ctor;
}

// A function in messages: "'f'", or what a class runs.
std::string calleeName(const FunctionDecl *fn) {
  if (!fn->cls) {
    return quotedName(fn->name);
  }
  if (fn == fn->cls->init || fn->isConstructor) {
    return "'new " + fn->cls->name.str() + "()'";
  }

  return "'" + fn->cls->name.str() + "." + fn->name.str() + "'";
}

} // namespace

LoadOrder::LoadOrder(Sema &sema) : sema_(sema) {}

// What the code that runs at some point may read: of the globals it uses,
// directly or through the functions it calls, the one set last. Functions
// the code only creates run later, and do not count; neither do calls
// through function values, which are not known here.
void LoadOrder::checkLoadOrder(SourceFile &program) {
  // When each global is set: the top-level declarations count up in the
  // order the program runs them.
  llvm::DenseMap<const VarSymbol *, unsigned> setAt;
  llvm::DenseMap<const VarSymbol *, Module *> homes;
  unsigned count = 0;
  for (Module *module : program.modules) {
    for (Stmt *item : module->file.items) {
      if (auto *decl = dyn_cast<VarDeclStmt>(item)) {
        for (const VarDeclarator &d : decl->decls) {
          if (d.sym) {
            setAt[d.sym] = ++count;
            homes[d.sym] = module;
          }
        }
      }
    }
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
    if (effects.try_emplace(fn).second) {
      queue.push_back(fn);
    }
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
        if (found == setAt.end() || found->second <= time) {
          return;
        }
        Module *home = homes.lookup(name->var);
        std::string what = "'" + name->name.str() + "'";
        if (name->name != name->var->name) {
          what += " ('" + name->var->name.str() + "' of " + home->name + ")";
        } else {
          what += " of " + home->name;
        }
        if (home == module) {
          sema_.diags_.error(name->loc(), "'" + name->name + "' is used before it is declared");
        } else {
          sema_.diags_.error(name->loc(), TopLevelChecker::readBeforeLoad(module, what, home, name->var->name));
        }
        return;
      }
      llvm::SmallVector<const FunctionDecl *, 2> constructed;
      auto *call = dyn_cast<CallExpr>(e);
      if (const CallExpr *ctor = constructorCall(e, constructed)) {
        call = const_cast<CallExpr *>(ctor);
      }
      if (!call) {
        return;
      }
      Site site{call, constructed, time, module};
      if (constructed.empty()) {
        calledFunctions(call, site.called);
      }
      if (site.called.empty()) {
        return;
      }
      for (const FunctionDecl *fn : site.called) {
        need(fn);
      }
      sites.push_back(std::move(site));
    };
    for (Stmt *item : module->file.items) {
      if (isa<FunctionDecl>(item)) {
        continue;
      }
      auto *decl = dyn_cast<VarDeclStmt>(item);
      if (!decl) {
        visitExprs(item, visit, /*intoFunctions=*/false);
        continue;
      }
      for (const VarDeclarator &d : decl->decls) {
        if (d.init) {
          visitExprs(d.init, visit, /*intoFunctions=*/false);
        }
        if (d.sym) {
          ++time;
        }
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
        if (found != setAt.end() && found->second > own.latest.setAt) {
          own.latest = {name->var, found->second, name->loc()};
        }
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
    for (const Param &param : fn->params) {
      if (param.defaultValue) {
        visitExprs(param.defaultValue, visit, /*intoFunctions=*/false);
      }
    }
    visitExprs(fn->body, visit, /*intoFunctions=*/false);
    for (const FunctionDecl *callee : own.calls) {
      need(callee);
    }
    effects[fn] = std::move(own);
  }
  // And with the functions they call, until nothing changes.
  for (bool changed = true; changed;) {
    changed = false;
    for (auto &entry : effects) {
      for (const FunctionDecl *callee : entry.second.calls) {
        const Use &theirs = effects.find(callee)->second.latest;
        if (theirs.setAt > entry.second.latest.setAt) {
          entry.second.latest = theirs;
          changed = true;
        }
      }
    }
  }

  for (const Site &site : sites) {
    for (const FunctionDecl *fn : site.called) {
      const Use &use = effects.find(fn)->second.latest;
      if (!use.var || use.setAt <= site.time) {
        continue;
      }
      Module *home = homes.lookup(use.var);
      std::string var = use.var->name.str();
      if (home == site.module) {
        sema_.diags_.error(site.call->loc(),
                     calleeName(fn) + " uses '" + var +
                         "' before it is declared: JavaScript would throw a "
                         "ReferenceError when it runs here; move the declaration of '" +
                         var + "' up");
      } else {
        sema_.diags_.error(site.call->loc(),
                     site.module->name + " calls " + calleeName(fn) +
                         " while it loads, and it reads '" + var + "' of " + home->name +
                         ", which is not loaded yet: " + TopLevelChecker::loadChain(site.module, home) +
                         "; make the call inside a function, or move '" + var +
                         "' to a file outside the cycle");
      }
      sema_.diags_.note(use.loc, "'" + var + "' is used here");
      sema_.diags_.note(use.var->loc, "'" + var + "' is declared here");
      break; // one message for the call
    }
  }
}

} // namespace tz
