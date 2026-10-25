#include "compiler/instances/constructor_calls/constructor_calls.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

ConstructorCalls::ConstructorCalls(Sema &sema) : sema_(sema) {}

// A call `this.m()` in a constructor (or in the initializers of the fields)
// runs before the classes down the chain have set their fields: if one of
// them overrides m, its m would read fields that are not set yet (node gives
// undefined, or throws a TypeError). The same through the methods the
// constructor calls on `this`.
void ConstructorCalls::checkConstructorCalls() {
  for (ClassDecl *cls : sema_.classes_) {
    // An instance of a generic class: its generic class reports.
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
        // A call of the constructor is reported once, whatever it leads to.
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
          // The class down the chain whose method would run.
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
          sema_.diags_.error(site->loc(),
                       what + " calls '" + target->name.str() + "'" +
                           (site == call ? std::string() : " (through the methods it calls)") +
                           ", which '" + by->name +
                           "' overrides: it would run "
                           "before the fields of '" +
                           by->name + "' are set; call it once the object is made");
          reportedCalls_.insert(site);
          return;
        }
        // The methods each call of the constructor leads to.
        if (!origin) {
          seen.clear();
        }
        seen.insert(fn);
        scanConstructorCalls(target, cls, site, ctor, seen);
      },
      /*intoFunctions=*/false);
}

} // namespace tz
