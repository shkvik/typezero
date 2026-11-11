#include "compiler/interprocedural/pool_checker/pool_checker.h"
#include "compiler/interprocedural/pool_checker/pool_checker_errors.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

PoolChecker::PoolChecker(Program &checked, const LibraryRanges &library)
    : HeldValueChecker(checked, library) {}

bool PoolChecker::used() const {
  return this->usesModule("tz:sync");
}

bool PoolChecker::poolClass(const ClassDecl *cls) {
  for (; cls; cls = cls->parent) {
    const ClassDecl *base = cls->generic() ? cls->generic() : cls;
    if ((base->library() || cls->library()) && base->name == "Pool") {
      return true;
    }
  }

  return false;
}

bool PoolChecker::takesOver(const CallExpr *c) {
  const FunctionDecl *decl = c->method() ? c->method()->decl : nullptr;
  if (!decl || decl->name != "use" || !PoolChecker::poolClass(decl->cls()) || c->args.empty()) {
    return false;
  }
  const FunctionDecl *fn = HeldValueChecker::callbackOf(c->args[0]);
  this->markParam(fn, 0);
  this->uses.push_back({c->args[0], fn});
  if (fn && !fn->params.empty() && fn->params[0].sym && fn->params[0].sym->type) {
    this->itemTypes.insert(fn->params[0].sym->type->nonOptional());
  }

  return true;
}

bool PoolChecker::produces(const CallExpr *c) const {
  return HeldValueChecker::storageMethod(c, "getStore") &&
         this->poolStores.contains(HeldValueChecker::storageOf(c));
}

bool PoolChecker::heldWithObject(const MemberExpr *m) const {
  return m->type && this->itemTypes.contains(m->type->nonOptional());
}

bool PoolChecker::storeValue(const Expr *value) const {
  const Type *type = value && value->type ? value->type->nonOptional() : nullptr;

  return type && this->storeTypes.contains(type);
}

size_t PoolChecker::extraKnown() const {
  return this->poolStores.size() + this->itemTypes.size() + this->storeTypes.size();
}

std::vector<Diagnostic> PoolChecker::sawCall(const CallExpr *c) {
  if (HeldValueChecker::storageMethod(c, "getStore") && this->current &&
      this->poolStores.contains(HeldValueChecker::storageOf(c))) {
    this->readsStore.insert(this->current);
  }
  if (c->builtin() == Builtin::SetTimeout || c->builtin() == Builtin::SetImmediate ||
      c->builtin() == Builtin::QueueMicrotask || c->builtin() == Builtin::NextTick) {
    this->timers.push_back({c, this->current});
  }
  if (!HeldValueChecker::storageMethod(c, "run") || c->args.empty()) {
    return {};
  }
  const Expr *store = HeldValueChecker::bare(c->args[0]);
  if (auto *n = dyn_cast<NewExpr>(store)) {
    const std::vector<Expr *> &args = n->ctor() ? n->ctor()->args : n->args;
    if (llvm::none_of(args, [&](const Expr *a) { return this->isHeld(a); })) {
      return {};
    }
    this->allowedStores.insert(n);
    if (!this->once(n)) {
      return {};
    }
    std::string message = ErrorTextPoolObject::runStore();
    auto error = Diagnostic{
        .kind = llvm::SourceMgr::DK_Error,
        .loc = n->loc(),
        .message = message,
    };

    return {error};
  }
  auto *literal = dyn_cast<ObjectLitExpr>(store);
  if (!literal || llvm::none_of(literal->props, [&](const ObjectLitExpr::Property &p) {
        return !p.spread() && this->isHeld(p.value);
      })) {
    return {};
  }
  if (const VarSymbol *storage = HeldValueChecker::storageOf(c)) {
    this->poolStores.insert(storage);
  }
  if (literal->type) {
    this->storeTypes.insert(literal->type->nonOptional());
  }
  this->allowedStores.insert(literal);
  this->runs.push_back({c, this->current});
  if (this->awaited.contains(c) || !this->once(c)) {
    return {};
  }
  std::string message = ErrorTextPoolObject::awaitRun();
  auto error = Diagnostic{
      .kind = llvm::SourceMgr::DK_Error,
      .loc = c->loc(),
      .message = message,
  };

  return {error};
}

std::vector<Diagnostic> PoolChecker::spreadKept(const Expr *value) {
  if (!this->storeValue(value) || !this->once(value)) {
    return {};
  }
  std::string message = ErrorTextPoolObject::storeCopy();
  auto error = Diagnostic{
      .kind = llvm::SourceMgr::DK_Error,
      .loc = value->loc(),
      .message = message,
  };

  return {error};
}

void PoolChecker::sawNew(const NewExpr *n) {
  if (!PoolChecker::poolClass(n->cls())) {
    return;
  }
  const std::vector<Expr *> &args = n->ctor() ? n->ctor()->args : n->args;
  if (args.empty()) {
    return;
  }
  auto *options = dyn_cast<ObjectLitExpr>(HeldValueChecker::bare(args[0]));
  if (!options) {
    return;
  }
  for (const ObjectLitExpr::Property &p : options->props) {
    if (!p.spread() && (p.name == "alive" || p.name == "reset")) {
      this->markParam(HeldValueChecker::callbackOf(p.value), 0);
    }
  }
}

std::vector<Diagnostic> PoolChecker::sawVoid(const CallExpr *c) {
  this->voids.push_back({c, this->current});
  bool usesObject = false;
  if (auto *member = dyn_cast_or_null<MemberExpr>(c->callee)) {
    usesObject = this->isHeld(member->object);
  }
  for (const Expr *arg : c->args) {
    usesObject = usesObject || this->isHeld(arg);
  }
  if (!usesObject || !this->once(c)) {
    return {};
  }
  std::string message = ErrorTextPoolObject::voidCall();
  auto error = Diagnostic{
      .kind = llvm::SourceMgr::DK_Error,
      .loc = c->loc(),
      .message = message,
  };

  return {error};
}

bool PoolChecker::closuresCarry() const {
  return true;
}

std::vector<Diagnostic> PoolChecker::kept(const Expr *value, bool closure, llvm::StringRef where) {
  if (this->storeValue(value)) {
    return {};
  }
  std::string message =
      closure ? ErrorTextPoolObject::keptFn(where) : ErrorTextPoolObject::kept(where);
  auto error = Diagnostic{
      .kind = llvm::SourceMgr::DK_Error,
      .loc = value->loc(),
      .message = message,
  };

  return {error};
}

std::vector<const FunctionDecl *> PoolChecker::runRoots(const Expr *runCallback) const {
  auto roots = std::vector<const FunctionDecl *>();
  auto pending = std::vector<PassedCallback>{PassedCallback{runCallback, 0}};
  while (!pending.empty()) {
    auto [callback, depth] = pending.back();
    pending.pop_back();
    const Expr *bareCallback = HeldValueChecker::bare(callback);
    if (depth > 8 || bareCallback == nullptr) {
      continue;
    }
    const auto *name = dyn_cast<NameExpr>(bareCallback);
    const VarSymbol *var = name != nullptr ? name->var() : nullptr;
    bool isParam = var != nullptr && var->owner() != nullptr && !var->lambda() && !var->function();
    if (isParam) {
      auto args = this->argsAtCallSites(var);
      for (const Expr *arg : llvm::reverse(args)) {
        pending.push_back(PassedCallback{arg, depth + 1});
      }
      continue;
    }
    const FunctionDecl *fn = HeldValueChecker::callbackOf(bareCallback);
    if (fn != nullptr) {
      roots.push_back(fn);
    }
  }

  return roots;
}

std::vector<const Expr *> PoolChecker::argsAtCallSites(const VarSymbol *param) const {
  auto args = std::vector<const Expr *>();
  const FunctionDecl *owner = param->owner();
  for (unsigned i = 0; i < owner->params.size(); ++i) {
    if (owner->params[i].sym != param) {
      continue;
    }
    for (const FunctionDecl *key : {owner, static_cast<const FunctionDecl *>(owner->generic)}) {
      auto sites = key ? this->callSites.find(key) : this->callSites.end();
      if (sites == this->callSites.end()) {
        continue;
      }
      for (const CallExpr *site : sites->second) {
        if (i < site->args.size()) {
          args.push_back(site->args[i]);
        }
      }
    }
  }

  return args;
}

const FunctionDecl *PoolChecker::storeReader(const FunctionDecl *fn) const {
  llvm::DenseMap<const FunctionDecl *, const FunctionDecl *> via;
  for (const FunctionDecl *f : this->readsStore) {
    via[f] = f;
  }
  for (bool grew = true; grew;) {
    grew = false;
    for (const auto &[f, targets] : this->callees) {
      if (via.count(f)) {
        continue;
      }
      for (const FunctionDecl *g : targets) {
        if (auto found = via.find(g); found != via.end()) {
          via[f] = found->second;
          grew = true;
          break;
        }
      }
    }
  }
  auto found = via.find(fn);

  return found == via.end() ? nullptr : found->second;
}

std::vector<Diagnostic> PoolChecker::findLateStoreReads() {
  if (this->runs.empty()) {
    return {};
  }
  std::vector<const FunctionDecl *> pending;
  for (const auto &[run, at] : this->runs) {
    if (run->args.size() > 1) {
      auto roots = this->runRoots(run->args[1]);
      llvm::append_range(pending, roots);
    }
  }
  llvm::DenseSet<const FunctionDecl *> inside;
  while (!pending.empty()) {
    const FunctionDecl *fn = pending.back();
    pending.pop_back();
    if (!fn || !inside.insert(fn).second) {
      continue;
    }
    if (auto found = this->callees.find(fn); found != this->callees.end()) {
      pending.insert(pending.end(), found->second.begin(), found->second.end());
    }
  }
  auto how = [](const FunctionDecl *fn, const FunctionDecl *reads) {
    llvm::StringRef through = reads == fn ? llvm::StringRef() : reads->name;

    return ErrorTextPoolObject::readsIt(through);
  };

  auto found = std::vector<Diagnostic>();
  for (const auto &[c, at] : this->voids) {
    if (!at || !inside.contains(at)) {
      continue;
    }
    const FunctionDecl *target = this->programTarget(c);
    if (!target) {
      target = HeldValueChecker::callbackOf(c->callee);
    }
    const FunctionDecl *reads = target ? this->storeReader(target) : nullptr;
    if (!reads || !this->once(c)) {
      continue;
    }
    std::string reading = how(target, reads);
    std::string message = ErrorTextPoolObject::voidStore(reading);
    found.push_back({
        .kind = llvm::SourceMgr::DK_Error,
        .loc = c->loc(),
        .message = message,
    });
  }
  for (const auto &[c, at] : this->timers) {
    if (!at || !inside.contains(at) || c->args.empty()) {
      continue;
    }
    const FunctionDecl *fn = HeldValueChecker::callbackOf(c->args[0]);
    const FunctionDecl *reads = fn ? this->storeReader(fn) : nullptr;
    if (!reads || !this->once(c->args[0])) {
      continue;
    }
    std::string reading = how(fn, reads);
    std::string message = ErrorTextPoolObject::later(reading);
    found.push_back({
        .kind = llvm::SourceMgr::DK_Error,
        .loc = c->args[0]->loc(),
        .message = message,
    });
  }

  return found;
}

std::vector<Diagnostic> PoolChecker::findReturnedItems() {
  auto found = std::vector<Diagnostic>();
  for (const auto &[at, fn] : this->uses) {
    if (!fn || !this->returnsHeld.contains(fn) || !this->once(at)) {
      continue;
    }
    std::string message = ErrorTextPoolObject::returned();
    found.push_back({
        .kind = llvm::SourceMgr::DK_Error,
        .loc = at->loc(),
        .message = message,
    });
  }

  return found;
}

std::vector<Diagnostic> PoolChecker::finish() {
  auto lateStoreReads = this->findLateStoreReads();
  auto returnedItems = this->findReturnedItems();

  auto found = std::vector<Diagnostic>();
  llvm::append_range(found, lateStoreReads);
  llvm::append_range(found, returnedItems);

  return found;
}

void PoolChecker::startWalk() {
  this->uses.clear();
  this->readsStore.clear();
  this->runs.clear();
  this->voids.clear();
  this->timers.clear();
}

}
