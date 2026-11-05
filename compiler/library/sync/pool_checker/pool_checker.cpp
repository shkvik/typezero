#include "compiler/library/sync/pool_checker/pool_checker.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr llvm::StringLiteral kSection = "(docs/async.md, section 11)";

}

PoolChecker::PoolChecker(Program &program) : HeldValueChecker(program) {}

bool PoolChecker::used() const { return usesModule("tz:sync"); }

bool PoolChecker::poolClass(const ClassDecl *cls) {
  for (; cls; cls = cls->parent) {
    const ClassDecl *base = cls->generic ? cls->generic : cls;
    if ((base->library || cls->library) && base->name == "Pool") {
      return true;
    }
  }

  return false;
}

bool PoolChecker::takesOver(const CallExpr *c) {
  const FunctionDecl *decl = c->method ? c->method->decl : nullptr;
  if (!decl || decl->name != "use" || !poolClass(decl->cls()) || c->args.empty()) {
    return false;
  }
  const FunctionDecl *fn = callbackOf(c->args[0]);
  markParam(fn, 0);
  uses_.push_back({c->args[0], fn});
  if (fn && !fn->params.empty() && fn->params[0].sym && fn->params[0].sym->type) {
    itemTypes_.insert(fn->params[0].sym->type->nonOptional());
  }

  return true;
}

bool PoolChecker::produces(const CallExpr *c) const {
  return storageMethod(c, "getStore") && poolStores_.contains(storageOf(c));
}

bool PoolChecker::heldMember(const MemberExpr *m) const {
  return m->type && itemTypes_.contains(m->type->nonOptional()) && isHeld(m->object);
}

bool PoolChecker::storeValue(const Expr *value) const {
  const Type *type = value && value->type ? value->type->nonOptional() : nullptr;

  return type && storeTypes_.contains(type);
}

size_t PoolChecker::extraKnown() const {
  return poolStores_.size() + itemTypes_.size() + storeTypes_.size();
}

void PoolChecker::sawCall(const CallExpr *c) {
  if (storageMethod(c, "getStore") && current_ && poolStores_.contains(storageOf(c))) {
    readsStore_.insert(current_);
  }
  if (c->builtin == Builtin::SetTimeout || c->builtin == Builtin::SetImmediate ||
      c->builtin == Builtin::QueueMicrotask || c->builtin == Builtin::NextTick) {
    timers_.push_back({c, current_});
  }
  if (!storageMethod(c, "run") || c->args.empty()) {
    return;
  }
  const Expr *store = bare(c->args[0]);
  if (auto *n = dyn_cast<NewExpr>(store)) {
    const std::vector<Expr *> &args = n->ctor ? n->ctor->args : n->args;
    if (llvm::none_of(args, [&](const Expr *a) { return isHeld(a); })) {
      return;
    }
    allowedStores_.insert(n);
    if (once(n)) {
      program_.diags->error(n->loc(), "a store that holds the object from a Pool is written as an "
                                      "object literal right in 'run', e.g. 'run({ connection }, "
                                      "fn)': then the compiler sees where the object goes " +
                                          kSection);
    }
    return;
  }
  auto *literal = dyn_cast<ObjectLitExpr>(store);
  if (!literal || llvm::none_of(literal->props, [&](const ObjectLitExpr::Property &p) {
        return !p.spread && isHeld(p.value);
      })) {
    return;
  }
  if (const VarSymbol *storage = storageOf(c)) {
    poolStores_.insert(storage);
  }
  if (literal->type) {
    storeTypes_.insert(literal->type->nonOptional());
  }
  allowedStores_.insert(literal);
  runs_.push_back({c, current_});
  if (!awaited_.contains(c) && once(c)) {
    program_.diags->error(c->loc(), "'run' with the object from a Pool in its store must be "
                                    "awaited (or returned) inside 'use': after 'use' the pool "
                                    "gives the object to another request, while the code inside "
                                    "'run' could still use it " +
                                        kSection);
  }
}

void PoolChecker::spreadKept(const Expr *value) {
  if (!storeValue(value) || !once(value)) {
    return;
  }
  program_.diags->error(value->loc(), "this store holds the object from a Pool, so a copy of it "
                                      "would keep the object after 'use'; copy the fields you "
                                      "need instead " +
                                          kSection);
}

void PoolChecker::sawNew(const NewExpr *n) {
  if (!poolClass(n->cls)) {
    return;
  }
  const std::vector<Expr *> &args = n->ctor ? n->ctor->args : n->args;
  if (args.empty()) {
    return;
  }
  auto *options = dyn_cast<ObjectLitExpr>(bare(args[0]));
  if (!options) {
    return;
  }
  for (const ObjectLitExpr::Property &p : options->props) {
    if (!p.spread && (p.name == "alive" || p.name == "reset")) {
      markParam(callbackOf(p.value), 0);
    }
  }
}

void PoolChecker::sawVoid(const CallExpr *c) {
  voids_.push_back({c, current_});
  bool uses = false;
  if (auto *member = dyn_cast_or_null<MemberExpr>(c->callee)) {
    uses = isHeld(member->object);
  }
  for (const Expr *arg : c->args) {
    uses = uses || isHeld(arg);
  }
  if (!uses || !once(c)) {
    return;
  }
  program_.diags->error(c->loc(), "'void' does not wait for this call, but it uses the object "
                                  "from a Pool, which 'use' gives to another request as soon as "
                                  "its function ends; await the call instead " +
                                      kSection);
}

bool PoolChecker::closuresCarry() const { return true; }

void PoolChecker::kept(const Expr *value, bool closure, const llvm::Twine &where) {
  if (storeValue(value)) {
    return;
  }
  if (closure) {
    program_.diags->error(value->loc(), "a function that uses the object from a Pool can't be "
                                        "kept " +
                                            where +
                                            ": it could run after 'use', when the pool has given "
                                            "the object to another request; take what you need "
                                            "from the object first " +
                                            kSection);
    return;
  }
  program_.diags->error(value->loc(), "the object from a Pool lives only inside 'use' (after it "
                                      "the pool gives it to another request), so it can't be "
                                      "kept " +
                                          where + " " + kSection);
}

void PoolChecker::runRoots(const Expr *callback, unsigned depth,
                           std::vector<const FunctionDecl *> &roots) {
  callback = bare(callback);
  if (depth > 8 || !callback) {
    return;
  }
  if (auto *name = dyn_cast<NameExpr>(callback);
      name && name->var && name->var->owner() && !name->var->lambda && !name->var->function) {
    const FunctionDecl *owner = name->var->owner();
    for (unsigned i = 0; i < owner->params.size(); ++i) {
      if (owner->params[i].sym != name->var) {
        continue;
      }
      for (const FunctionDecl *key : {owner, static_cast<const FunctionDecl *>(owner->generic)}) {
        auto sites = key ? callSites_.find(key) : callSites_.end();
        if (sites == callSites_.end()) {
          continue;
        }
        for (const CallExpr *site : sites->second) {
          if (i < site->args.size()) {
            runRoots(site->args[i], depth + 1, roots);
          }
        }
      }
    }
    return;
  }
  if (const FunctionDecl *fn = callbackOf(callback)) {
    roots.push_back(fn);
  }
}

const FunctionDecl *PoolChecker::storeReader(const FunctionDecl *fn) const {
  llvm::DenseMap<const FunctionDecl *, const FunctionDecl *> via;
  for (const FunctionDecl *f : readsStore_) {
    via[f] = f;
  }
  for (bool grew = true; grew;) {
    grew = false;
    for (const auto &[f, targets] : callees_) {
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

void PoolChecker::checkStoreRuns() {
  if (runs_.empty()) {
    return;
  }
  std::vector<const FunctionDecl *> pending;
  for (const auto &[run, at] : runs_) {
    if (run->args.size() > 1) {
      runRoots(run->args[1], 0, pending);
    }
  }
  llvm::DenseSet<const FunctionDecl *> inside;
  while (!pending.empty()) {
    const FunctionDecl *fn = pending.back();
    pending.pop_back();
    if (!fn || !inside.insert(fn).second) {
      continue;
    }
    if (auto found = callees_.find(fn); found != callees_.end()) {
      pending.insert(pending.end(), found->second.begin(), found->second.end());
    }
  }
  auto how = [](const FunctionDecl *fn, const FunctionDecl *reads) {
    return reads == fn || reads->name.empty()
               ? std::string("it reads the store with getStore()")
               : "it reads the store through '" + reads->name.str() + "'";
  };
  for (const auto &[c, at] : voids_) {
    if (!at || !inside.contains(at)) {
      continue;
    }
    const FunctionDecl *target = programTarget(c);
    if (!target) {
      target = callbackOf(c->callee);
    }
    const FunctionDecl *reads = target ? storeReader(target) : nullptr;
    if (!reads || !once(c)) {
      continue;
    }
    program_.diags->error(c->loc(), "'void' does not wait for this call, but " +
                                        how(target, reads) +
                                        ", and this code can run inside 'run' whose store holds "
                                        "the object from a Pool, which 'use' gives to another "
                                        "request as soon as its function ends; await the call "
                                        "instead " +
                                        kSection);
  }
  for (const auto &[c, at] : timers_) {
    if (!at || !inside.contains(at) || c->args.empty()) {
      continue;
    }
    const FunctionDecl *fn = callbackOf(c->args[0]);
    const FunctionDecl *reads = fn ? storeReader(fn) : nullptr;
    if (!reads || !once(c->args[0])) {
      continue;
    }
    program_.diags->error(c->args[0]->loc(),
                          "this function runs later, but " + how(fn, reads) +
                              ", and this code can run inside 'run' whose store holds the object "
                              "from a Pool: by then 'use' may have given the object to another "
                              "request; do the work before, or outside the transaction " +
                              kSection);
  }
}

void PoolChecker::finish() {
  checkStoreRuns();
  for (const auto &[at, fn] : uses_) {
    if (!fn || !returnsHeld_.contains(fn) || !once(at)) {
      continue;
    }
    program_.diags->error(at->loc(), "this function returns the object from the pool, but "
                                     "after 'use' the pool gives it to another request; return "
                                     "what you need from it instead " +
                                         kSection);
  }
}

void PoolChecker::startWalk() {
  uses_.clear();
  readsStore_.clear();
  runs_.clear();
  voids_.clear();
  timers_.clear();
}
}
