#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/parallel/explainer/explainer.h"
#include "compiler/parallel/expr_walk/expr_walk.h"
#include "compiler/parallel/library_effects/library_effects.h"
#include "compiler/parallel/ownership/ownership.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/parallel/pieces/pieces.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz::parallel {

CallResolver::CallResolver(Analysis &analysis) : analysis_(analysis) {}

void CallResolver::resolve() {
  while (true) {
    while (!analysis_.worklist.empty()) {
      unsigned fi = analysis_.worklist.front();
      analysis_.worklist.pop_front();
      analysis_.codeWalk().walk(fi);
    }
    std::map<const Type *, std::vector<unsigned>> generators;
    for (unsigned fi = 0; fi < analysis_.fns.size(); ++fi) {
      const FnInfo &f = analysis_.fns[fi];
      if ((f.syncGen || f.asyncGen) && f.decl->sym && f.decl->sym->ret) {
        generators[f.decl->sym->ret].push_back(fi);
      }
    }
    for (unsigned fi = 0; fi < analysis_.fns.size(); ++fi) {
      FnInfo &caller = analysis_.fns[fi];
      for (CallRec &rec : caller.calls) {
        if (rec.reflect == CallRec::Reflect::Apply && rec.noSelf) {
          const llvm::DenseSet<unsigned> &boxed =
              analysis_.unknownAnyFn ? analysis_.valueFns : analysis_.unknownFns;
          std::vector<unsigned> values(boxed.begin(), boxed.end());
          std::sort(values.begin(), values.end());
          for (unsigned g : values) {
            analysis_.callWalk().addCallee(rec, analysis_.fns[g].decl,
                                           analysis_.classRefOrNone(analysis_.fns[g]), true);
          }
          continue;
        }
        if (rec.reflect != CallRec::Reflect::None) {
          std::vector<const ClassDecl *> classes = analysis_.programClasses;
          for (const ClassDecl *cls : analysis_.classes) {
            if (!cls->library && !llvm::is_contained(classes, cls)) {
              classes.push_back(cls);
            }
          }
          for (const ClassDecl *cls : classes) {
            if (cls->generic) {
              continue;
            }
            if (rec.reflect == CallRec::Reflect::Apply) {
              const Type *object = rec.byObject ? rec.byObject->nonOptional() : nullptr;
              if (object && object->isObject() && !object->isAnyObject() && cls->type &&
                  !analysis_.aliasAnalysis().typesMayAlias(cls->type, object)) {
                continue;
              }
              for (const FunctionDecl *method : cls->methods) {
                if (!method->isStatic && (rec.reflectNames.empty() ||
                                          llvm::is_contained(rec.reflectNames, method->name))) {
                  analysis_.callWalk().addCallee(rec, method, rec.bySelf, true);
                }
              }
              continue;
            }
            bool hidden = !cls->constructors.empty() &&
                          llvm::all_of(cls->constructors, [](const FunctionDecl *ctor) {
                            return ctor->access != tz::Access::Public;
                          });
            if (hidden) {
              continue;
            }
            std::optional<Ref> self = Ref{RefKind::Site, rec.site};
            for (const FunctionDecl *ctor : cls->constructors) {
              analysis_.callWalk().addCallee(rec, ctor, self, true);
            }
            for (const ClassDecl *k = cls; k; k = k->parent) {
              if (k->init) {
                analysis_.callWalk().addCallee(rec, k->init, self, true);
              }
            }
          }
          continue;
        }
        if (rec.channel >= 0) {
          for (unsigned h : std::vector<unsigned>(analysis_.listeners[rec.channel])) {
            analysis_.callWalk().addCallee(rec, analysis_.fns[h].decl,
                                           analysis_.classRefOrNone(analysis_.fns[h]), true);
          }
          for (const Type *t : analysis_.listenerTypes[rec.channel]) {
            auto found = analysis_.values.find(t->nonOptional());
            if (found != analysis_.values.end()) {
              for (unsigned g : std::vector<unsigned>(found->second)) {
                analysis_.callWalk().addCallee(rec, analysis_.fns[g].decl,
                                               analysis_.classRefOrNone(analysis_.fns[g]), true);
              }
            }
          }
          continue;
        }
        if (!rec.byType) {
          continue;
        }
        if (rec.kind == CallKind::IterSync || rec.kind == CallKind::IterAsync) {
          auto found = generators.find(rec.byType->nonOptional());
          if (found != generators.end()) {
            for (unsigned g : found->second) {
              analysis_.callWalk().addCallee(rec, analysis_.fns[g].decl,
                                             analysis_.classRefOrNone(analysis_.fns[g]), true);
            }
          }
          continue;
        }
        auto found = analysis_.values.find(rec.byType->nonOptional());
        if (found != analysis_.values.end()) {
          for (unsigned g : std::vector<unsigned>(found->second)) {
            std::optional<Ref> self;
            if (analysis_.fns[g].thisOwner) {
              self = analysis_.callWalk().classRef(analysis_.fns[g].thisOwner);
            }
            analysis_.callWalk().addCallee(rec, analysis_.fns[g].decl, self, true);
          }
        }
        if (!rec.byName.empty() && rec.byObject) {
          const Type *object = rec.byObject->nonOptional();
          for (size_t k = 0; k < analysis_.classes.size(); ++k) {
            const ClassDecl *cls = analysis_.classes[k];
            if (!cls->type ||
                !(cls->literal || analysis_.aliasAnalysis().typesMayAlias(cls->type, object))) {
              continue;
            }
            for (const FunctionDecl *method : cls->methods) {
              if (method->name == rec.byName && !method->isStatic) {
                analysis_.callWalk().addCallee(rec, method, rec.bySelf, true);
              }
            }
          }
        }
      }
    }
    if (analysis_.worklist.empty()) {
      break;
    }
  }
}

std::optional<Access> CallResolver::substitute(const Access &a, const FnInfo &callee,
                                               unsigned caller, const Callee &call,
                                               const CallRec &rec) const {
  const std::optional<Ref> &self = call.self;
  Access b = a;
  if (b.bound && callee.decl) {
    const std::vector<Param> &params = callee.decl->params;
    auto it = llvm::find_if(params, [&](const Param &p) { return p.sym == b.bound; });
    if (it != params.end()) {
      const Expr *arg = argument(rec, size_t(it - params.begin()));
      const FunctionDecl *passed = functionOf(arg);
      const VarSymbol *param = callableParam(analysis_.fns[caller].decl, arg);
      if (passed && passed != b.through) {
        return std::nullopt;
      }
      if (param && !passed) {
        b.bound = param;
      } else {
        b.bound = nullptr;
        b.through = nullptr;
      }
    }
  }
  bool byLibrary = rec.kind == CallKind::Inline && rec.call &&
                   analysis_.libraryEffects().libraryTarget(rec.call);
  if (analysis_.isLibrary(callee.decl) || byLibrary) {
    b.origin = llvm::SMLoc();
    b.anchor = llvm::SMLoc();
    b.home = nullptr;
  } else if (!a.origin.isValid()) {
    b.origin = a.at;
    b.home = callee.decl;
  }
  b.at = rec.loc;
  b.via = callee.decl;
  if (b.loc.ref.kind == RefKind::Owned) {
    std::optional<Ref> ref =
        analysis_.ownership().substituted(b.loc.ref, callee, caller, rec, call);
    if (!ref) {
      return std::nullopt;
    }
    b.loc.ref = *ref;
  }
  if (b.loc.ref.kind == RefKind::This) {
    auto *owner = static_cast<const FunctionDecl *>(b.loc.ref.ptr);
    if (owner == callee.thisOwner && self) {
      if (self->kind == RefKind::Local) {
        return std::nullopt;
      }
      b.loc.ref = *self;
    } else if (owner != callee.thisOwner || !self) {
      b.loc.ref = analysis_.callWalk().classRef(owner);
    }
  }

  return b;
}

void CallResolver::fold() {
  for (unsigned fi = 0; fi < analysis_.fns.size(); ++fi) {
    FnInfo &f = analysis_.fns[fi];
    for (unsigned s = 0; s < f.segs.size(); ++s) {
      if (f.isInline(s)) {
        f.inlineEffects.insert(f.segs[s].effects.begin(), f.segs[s].effects.end());
      }
    }
    for (unsigned r = 0; r < f.calls.size(); ++r) {
      for (const Callee &c : f.calls[r].callees) {
        analysis_.fns[c.fn].named.push_back({fi, r});
        if (c.mode != Mode::Callback) {
          analysis_.fns[c.fn].callers.push_back({fi, r});
        }
      }
    }
  }
  std::deque<unsigned> queue;
  std::vector<bool> queued(analysis_.fns.size(), true);
  for (unsigned fi = 0; fi < analysis_.fns.size(); ++fi) {
    queue.push_back(fi);
  }
  while (!queue.empty()) {
    unsigned fi = queue.front();
    queue.pop_front();
    queued[fi] = false;
    FnInfo &f = analysis_.fns[fi];
    bool grew = false;
    for (const CallRec &rec : f.calls) {
      for (const Callee &c : rec.callees) {
        if (c.mode == Mode::Callback) {
          continue;
        }
        const FnInfo &g = analysis_.fns[c.fn];
        std::vector<Access> added;
        for (const Access &a : g.inlineEffects) {
          if (std::optional<Access> b = substitute(a, g, fi, c, rec)) {
            if (rec.viaParam && !b->bound) {
              b->bound = rec.viaParam;
              b->through = g.decl;
            }
            added.push_back(*b);
          }
        }
        for (const Access &b : added) {
          for (unsigned s : rec.segs) {
            if (f.segs[s].effects.insert(b).second && f.isInline(s)) {
              grew |= f.inlineEffects.insert(b).second;
            }
          }
        }
      }
    }
    if (!grew) {
      continue;
    }
    for (const auto &[caller, r] : f.callers) {
      if (!queued[caller]) {
        queued[caller] = true;
        queue.push_back(caller);
      }
    }
  }
}

const Expr *CallResolver::argument(const CallRec &rec, size_t index) {
  if (rec.ownArgs > 0 || rec.kind == CallKind::Later) {
    return nullptr;
  }
  if (rec.call) {
    if (rec.call->hasSpread() || index >= rec.call->args.size()) {
      return nullptr;
    }
    return rec.call->args[index];
  }
  auto *n = dyn_cast_or_null<NewExpr>(static_cast<const Expr *>(rec.site));
  if (!n) {
    return nullptr;
  }
  const std::vector<Expr *> &args = n->ctor ? n->ctor->args : n->args;

  return index < args.size() ? args[index] : nullptr;
}

const FunctionDecl *CallResolver::functionOf(const Expr *e) {
  e = strip(e);
  if (auto *fe = dyn_cast_or_null<FunctionExpr>(e)) {
    return fe->fn;
  }
  auto *n = dyn_cast_or_null<NameExpr>(e);
  if (n && n->func) {
    return n->func->decl;
  }
  if (n && n->var && !n->var->reassigned && (n->var->lambda || n->var->function)) {
    return n->var->lambda ? n->var->lambda : n->var->function;
  }

  return nullptr;
}

const VarSymbol *CallResolver::callableParam(const FunctionDecl *fn, const Expr *e) {
  auto *n = dyn_cast_or_null<NameExpr>(strip(e));
  if (!fn || !n || !n->var || n->var->reassigned) {
    return nullptr;
  }
  bool param = llvm::any_of(fn->params, [&](const Param &p) { return p.sym == n->var; });

  return param ? n->var : nullptr;
}

std::optional<Ref> CallResolver::bind(const std::optional<Ref> &self,
                                      const Activation &caller) const {
  if (!self || self->kind != RefKind::This) {
    return self;
  }
  auto *owner = static_cast<const FunctionDecl *>(self->ptr);
  if (owner == analysis_.fns[caller.fn].thisOwner && caller.self) {
    return caller.self;
  }

  return analysis_.callWalk().classRef(owner);
}

}
