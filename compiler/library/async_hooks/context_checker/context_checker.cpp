#include "compiler/library/async_hooks/context_checker/context_checker.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/program/types/types.h"

using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kSection = "(docs/async.md, section 10)";

}

ContextChecker::ContextChecker(Program &program) : HeldValueChecker(program) {}

bool ContextChecker::used() const { return usesModule("node:async_hooks"); }

bool ContextChecker::produces(const CallExpr *c) const { return storageMethod(c, "getStore"); }

bool ContextChecker::takesOver(const CallExpr *c) {
  if (current_ && storageMethod(c, "getStore")) {
    readsDirectly_.insert(current_);
  }
  if (storageMethod(c, "run")) {
    runs_.push_back(c);
    return true;
  }
  if (c->builtin == Builtin::SetInterval ||
      (c->builtin == Builtin::SetTimeout && c->args.size() > 2)) {
    intervals_.push_back(c);
  }

  return false;
}

void ContextChecker::sawNew(const NewExpr *n) {
  if (storageClass(n->cls)) {
    news_.push_back(n);
  }
}

void ContextChecker::startWalk() {
  readsDirectly_.clear();
  intervals_.clear();
  runs_.clear();
  news_.clear();
}

void ContextChecker::kept(const Expr *value, bool closure, const llvm::Twine &where) {
  (void)closure;
  program_.diags->error(value->loc(), "the store of an AsyncLocalStorage lives only inside "
                                      "'run', so it can't be kept " +
                                          where + ": copy the fields you need instead " + kSection);
}

void ContextChecker::finish() {
  checkNews();
  checkRuns();
  checkIntervals();
}

void ContextChecker::checkNews() {
  for (const NewExpr *n : news_) {
    if (allowedNews_.contains(n)) {
      continue;
    }
    program_.diags->error(n->loc(), "an AsyncLocalStorage is made only as a 'const' at the top "
                                    "level of a module, e.g. 'export const requestContext = new "
                                    "AsyncLocalStorage<RequestContext>()' " +
                                        kSection);
  }
}

void ContextChecker::checkRuns() {
  for (const CallExpr *c : runs_) {
    if (c->args.empty() || !parallel::holdsReferences(c->args[0]->type)) {
      continue;
    }
    const Expr *store = bare(c->args[0]);
    if (isa<ObjectLitExpr, ArrayLitExpr, NewExpr, UndefinedExpr>(store)) {
      continue;
    }
    program_.diags->error(store->loc(), "the store given to 'run' is a new object written right "
                                        "here, e.g. 'run({ requestId }, ...)': an object that "
                                        "already exists could get into two runs at once " +
                                            kSection);
  }
}

const FunctionDecl *ContextChecker::reader(const FunctionDecl *fn) const {
  llvm::DenseMap<const FunctionDecl *, const FunctionDecl *> via;
  for (const FunctionDecl *f : readsDirectly_) {
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

void ContextChecker::checkIntervals() {
  for (const CallExpr *c : intervals_) {
    const FunctionDecl *fn = c->args.empty() ? nullptr : callbackOf(c->args[0]);
    const FunctionDecl *reads = fn ? reader(fn) : nullptr;
    if (!reads) {
      continue;
    }
    std::string how = reads == fn || reads->name.empty()
                          ? std::string("this function reads it with getStore()")
                          : "this function reads it through '" + reads->name.str() + "'";
    if (c->builtin == Builtin::SetInterval) {
      program_.diags->error(c->args[0]->loc(),
                            "setInterval does not carry the store of an AsyncLocalStorage (an "
                            "interval outlives 'run'), but " +
                                how +
                                "; read what it needs before setInterval and keep it in the "
                                "interval's object " +
                                kSection);
    } else {
      program_.diags->error(c->args[0]->loc(),
                            "a setTimeout that restarts itself does not carry the store of an "
                            "AsyncLocalStorage (like an interval, it outlives 'run'), but " +
                                how + "; read what it needs before and keep it in a field " +
                                kSection);
    }
  }
}

const ClassDecl *ContextChecker::storageClass(const ClassDecl *cls) {
  for (; cls; cls = cls->parent) {
    const ClassDecl *base = cls->generic ? cls->generic : cls;
    if ((base->library || cls->library) && base->name == "AsyncLocalStorage") {
      return base;
    }
  }

  return nullptr;
}

bool ContextChecker::storageMethod(const CallExpr *c, llvm::StringRef name) {
  const FunctionDecl *decl = c->method ? c->method->decl : nullptr;

  return decl && decl->name == name && storageClass(decl->cls);
}

}
