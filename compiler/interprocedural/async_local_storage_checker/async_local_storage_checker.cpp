#include "compiler/interprocedural/async_local_storage_checker/async_local_storage_checker.h"
#include "compiler/interprocedural/async_local_storage_checker/async_local_storage_checker_errors.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/program/types/types.h"

using llvm::isa;

namespace tz {

AsyncLocalStorageChecker::AsyncLocalStorageChecker(Program &checked, const LibraryRanges &library)
    : HeldValueChecker(checked, library) {}

bool AsyncLocalStorageChecker::used() const {
  return this->usesModule("node:async_hooks");
}

bool AsyncLocalStorageChecker::produces(const CallExpr *c) const {
  return HeldValueChecker::storageMethod(c, "getStore");
}

bool AsyncLocalStorageChecker::takesOver(const CallExpr *c) {
  if (this->current && HeldValueChecker::storageMethod(c, "getStore")) {
    this->readsDirectly.insert(this->current);
  }
  if (HeldValueChecker::storageMethod(c, "run")) {
    this->runs.push_back(c);
    return true;
  }
  if (c->builtin() == Builtin::SetInterval ||
      (c->builtin() == Builtin::SetTimeout && c->args.size() > 2)) {
    this->intervals.push_back(c);
  }

  return false;
}

void AsyncLocalStorageChecker::sawNew(const NewExpr *n) {
  if (HeldValueChecker::storageClass(n->cls())) {
    this->news.push_back(n);
  }
}

void AsyncLocalStorageChecker::startWalk() {
  this->readsDirectly.clear();
  this->intervals.clear();
  this->runs.clear();
  this->news.clear();
}

void AsyncLocalStorageChecker::kept(const Expr *value, bool, llvm::StringRef where) {
  std::string message = ErrorTextAsyncStore::kept(where);
  this->program.diags.error(value->loc(), message);
}

void AsyncLocalStorageChecker::finish() {
  this->checkNews();
  this->checkRuns();
  this->checkIntervals();
}

void AsyncLocalStorageChecker::checkNews() {
  for (const NewExpr *n : this->news) {
    if (this->allowedNews.contains(n)) {
      continue;
    }
    std::string message = ErrorTextAsyncStore::made();
    this->program.diags.error(n->loc(), message);
  }
}

void AsyncLocalStorageChecker::checkRuns() {
  for (const CallExpr *c : this->runs) {
    if (c->args.empty() || !parallel::ParallelHelpers::holdsReferences(c->args[0]->type)) {
      continue;
    }
    const Expr *store = HeldValueChecker::bare(c->args[0]);
    if (isa<ObjectLitExpr, ArrayLitExpr, NewExpr, UndefinedExpr>(store)) {
      continue;
    }
    std::string message = ErrorTextAsyncStore::runStore();
    this->program.diags.error(store->loc(), message);
  }
}

const FunctionDecl *AsyncLocalStorageChecker::reader(const FunctionDecl *fn) const {
  llvm::DenseMap<const FunctionDecl *, const FunctionDecl *> via;
  for (const FunctionDecl *f : this->readsDirectly) {
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

void AsyncLocalStorageChecker::checkIntervals() {
  for (const CallExpr *c : this->intervals) {
    const FunctionDecl *fn = c->args.empty() ? nullptr : HeldValueChecker::callbackOf(c->args[0]);
    const FunctionDecl *reads = fn ? this->reader(fn) : nullptr;
    if (!reads) {
      continue;
    }
    llvm::StringRef through = reads == fn ? llvm::StringRef() : reads->name;
    std::string how = ErrorTextAsyncStore::readsIt(through);
    std::string message = c->builtin() == Builtin::SetInterval ? ErrorTextAsyncStore::interval(how)
                                                               : ErrorTextAsyncStore::restart(how);
    this->program.diags.error(c->args[0]->loc(), message);
  }
}

}
