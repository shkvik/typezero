#include "compiler/instances/constructor_calls/constructor_calls.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

ConstructorCalls::ConstructorCalls(Checker &checker) : checker_(checker) {}

void ConstructorCalls::checkConstructorCalls() {
  for (ClassDecl *cls : checker_.classes_) {
    if (cls->literal || cls->children.empty() || cls->generic) {
      continue;
    }
    std::vector<FunctionDecl *> fns = cls->constructors;
    if (cls->init) {
      fns.push_back(cls->init);
    }
    for (FunctionDecl *ctor : fns) {
      llvm::SmallPtrSet<const FunctionDecl *, 8> seen;
      scanConstructorCalls(ctor, cls, nullptr, ctor, seen);
    }
  }
}

void ConstructorCalls::scanConstructorCalls(const FunctionDecl *fn, const ClassDecl *cls,
                                            const CallExpr *origin, const FunctionDecl *ctor,
                                            llvm::SmallPtrSetImpl<const FunctionDecl *> &seen) {
  if (!fn->body || !fn->thisVar || !seen.insert(fn).second) {
    return;
  }
  const VarSymbol *self = fn->thisVar;
  visitExprs(
      fn->body,
      [&](const Expr *e) {
        auto *call = dyn_cast<CallExpr>(e);
        if (!call || !call->method || (origin && reportedCalls_.contains(origin))) {
          return;
        }
        auto *member = dyn_cast<MemberExpr>(call->callee);
        const Expr *object = member ? member->object : nullptr;
        while (auto *conv = llvm::dyn_cast_or_null<CastExpr>(object)) {
          object = conv->operand;
        }
        auto *name = llvm::dyn_cast_or_null<NameExpr>(object);
        if (!name || name->var != self) {
          return;
        }
        FunctionDecl *target = call->method->decl;
        const CallExpr *site = origin ? origin : call;
        if (call->virtualCall || !cls->derivesFrom(target->cls)) {
          const ClassDecl *by = target->cls;
          if (call->virtualCall) {
            std::function<const ClassDecl *(const ClassDecl *)> find =
                [&](const ClassDecl *c) -> const ClassDecl * {
              for (const ClassDecl *child : c->children) {
                for (FunctionDecl *other : ClassLinks::ownMethods(child)) {
                  if (ClassLinks::overridesTransitively(other, target)) {
                    return child;
                  }
                }
                if (const ClassDecl *found = find(child)) {
                  return found;
                }
              }

              return nullptr;
            };
            if (const ClassDecl *found = find(cls)) {
              by = found;
            }
          }
          std::string what = ctor->isConstructor
                                 ? "the constructor of '" + cls->name.str() + "'"
                                 : "the initializers of the fields of '" + cls->name.str() + "'";
          checker_.diags_.error(
              site->loc(), what + " calls '" + target->name.str() + "'" +
                               (site == call ? std::string() : " (through the methods it calls)") +
                               ", which '" + by->name +
                               "' overrides: it would run "
                               "before the fields of '" +
                               by->name + "' are set; call it once the object is made");
          reportedCalls_.insert(site);
          return;
        }
        if (!origin) {
          seen.clear();
        }
        seen.insert(fn);
        scanConstructorCalls(target, cls, site, ctor, seen);
      },
      false);
}

}
