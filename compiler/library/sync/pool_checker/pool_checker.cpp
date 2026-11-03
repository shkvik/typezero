#include "compiler/library/sync/pool_checker/pool_checker.h"
#include "compiler/program/diagnostics/diagnostics.h"

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
  if (!decl || decl->name != "use" || !poolClass(decl->cls) || c->args.empty()) {
    return false;
  }
  const FunctionDecl *fn = callbackOf(c->args[0]);
  markParam(fn, 0);
  uses_.push_back({c->args[0], fn});

  return true;
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

void PoolChecker::finish() {
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

void PoolChecker::startWalk() { uses_.clear(); }

}
