#include "compiler/interprocedural/async_local_storage_checker/async_local_storage_checker.h"
#include "compiler/interprocedural/async_local_storage_checker/async_local_storage_checker_errors.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"

using llvm::isa;

namespace tz {

AsyncLocalStorageChecker::AsyncLocalStorageChecker(Program &checked, const LibraryRanges &library)
    : HeldValueChecker(checked, library) {}

bool AsyncLocalStorageChecker::used() const {
  return this->usesModule("node:async_hooks");
}

bool AsyncLocalStorageChecker::produces(const CallExpr *c) const {
  return this->storageMethod(c, "getStore");
}

bool AsyncLocalStorageChecker::takesOver(const CallExpr *c) {
  if (this->current && this->storageMethod(c, "getStore")) {
    this->readsDirectly.insert(this->current);
  }
  if (this->storageMethod(c, "run")) {
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
  if (this->storageClass(n->cls())) {
    this->news.push_back(n);
  }
}

std::vector<Diagnostic> AsyncLocalStorageChecker::sawVoid(const CallExpr *) {
  return {};
}

std::vector<Diagnostic> AsyncLocalStorageChecker::sawCall(const CallExpr *) {
  return {};
}

bool AsyncLocalStorageChecker::heldWithObject(const MemberExpr *) const {
  return false;
}

std::vector<Diagnostic> AsyncLocalStorageChecker::spreadKept(const Expr *) {
  return {};
}

size_t AsyncLocalStorageChecker::extraKnown() const {
  return 0;
}

bool AsyncLocalStorageChecker::closuresCarry() const {
  return false;
}

void AsyncLocalStorageChecker::startWalk() {
  this->readsDirectly.clear();
  this->intervals.clear();
  this->runs.clear();
  this->news.clear();
}

std::vector<Diagnostic> AsyncLocalStorageChecker::kept(const Expr *value, bool,
                                                       llvm::StringRef where) {
  std::string message = ErrorTextAsyncStore::kept(where);
  auto error = Diagnostic{
      .kind = llvm::SourceMgr::DK_Error,
      .loc = value->loc(),
      .message = message,
  };

  return {error};
}

std::vector<Diagnostic> AsyncLocalStorageChecker::finish() {
  auto misplacedNews = this->findMisplacedNews();
  auto reusedStores = this->findReusedStores();
  auto readingTimers = this->findStoreReadingTimers();

  auto found = std::vector<Diagnostic>();
  llvm::append_range(found, misplacedNews);
  llvm::append_range(found, reusedStores);
  llvm::append_range(found, readingTimers);

  return found;
}

std::vector<Diagnostic> AsyncLocalStorageChecker::findMisplacedNews() const {
  auto found = std::vector<Diagnostic>();
  for (const NewExpr *n : this->news) {
    if (this->allowedNews.contains(n)) {
      continue;
    }
    std::string message = ErrorTextAsyncStore::made();
    found.push_back({
        .kind = llvm::SourceMgr::DK_Error,
        .loc = n->loc(),
        .message = message,
    });
  }

  return found;
}

std::vector<Diagnostic> AsyncLocalStorageChecker::findReusedStores() const {
  auto found = std::vector<Diagnostic>();
  for (const CallExpr *c : this->runs) {
    if (c->args.empty() || !parallel::ParallelHelpers::holdsReferences(c->args[0]->type)) {
      continue;
    }
    const Expr *store = this->bare(c->args[0]);
    if (isa<ObjectLitExpr, ArrayLitExpr, NewExpr, UndefinedExpr>(store)) {
      continue;
    }
    std::string message = ErrorTextAsyncStore::runStore();
    found.push_back({
        .kind = llvm::SourceMgr::DK_Error,
        .loc = store->loc(),
        .message = message,
    });
  }

  return found;
}

const FunctionDecl *AsyncLocalStorageChecker::reader(const FunctionDecl *fn) const {
  llvm::DenseMap<const FunctionDecl *, const FunctionDecl *> via;
  for (const FunctionDecl *f : this->readsDirectly) {
    via[f] = f;
  }
  for (bool hasGrown = true; hasGrown;) {
    hasGrown = false;
    for (const auto &[f, targets] : this->callees) {
      if (via.count(f)) {
        continue;
      }
      for (const FunctionDecl *g : targets) {
        if (auto found = via.find(g); found != via.end()) {
          via[f] = found->second;
          hasGrown = true;
          break;
        }
      }
    }
  }
  auto found = via.find(fn);

  return found == via.end() ? nullptr : found->second;
}

std::vector<Diagnostic> AsyncLocalStorageChecker::findStoreReadingTimers() const {
  auto found = std::vector<Diagnostic>();
  for (const CallExpr *c : this->intervals) {
    const FunctionDecl *fn = c->args.empty() ? nullptr : this->callbackOf(c->args[0]);
    const FunctionDecl *reads = fn ? this->reader(fn) : nullptr;
    if (!reads) {
      continue;
    }
    llvm::StringRef through = reads == fn ? llvm::StringRef() : reads->name;
    std::string how = ErrorTextAsyncStore::readsIt(through);
    std::string message = c->builtin() == Builtin::SetInterval ? ErrorTextAsyncStore::interval(how)
                                                               : ErrorTextAsyncStore::restart(how);
    found.push_back({
        .kind = llvm::SourceMgr::DK_Error,
        .loc = c->args[0]->loc(),
        .message = message,
    });
  }

  return found;
}

}
