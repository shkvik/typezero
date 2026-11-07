#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/analysis/analysis.h"
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

CallResolver::CallResolver(Analysis &analysis)
    : analysis_(analysis) {}

void CallResolver::resolve() {
  while (true) {
    while (!analysis_.worklist.empty()) {
      int fi = analysis_.worklist.front();
      analysis_.worklist.pop_front();
      analysis_.codeWalk.walk(fi);
    }
    std::map<const Type *, std::vector<int>> generators;
    for (size_t i = 0; i < analysis_.records.size(); ++i) {
      const FnInfo &f = analysis_.records[i];
      if ((f.syncGen || f.asyncGen) && f.decl->sym && f.decl->sym->ret) {
        generators[f.decl->sym->ret].push_back(f.decl->id);
      }
    }
    for (size_t i = 0; i < analysis_.records.size(); ++i) {
      FnInfo &caller = analysis_.records[i];
      for (CallRec &rec : caller.calls) {
        if (rec.reflect == CallRec::Reflect::Apply && rec.noSelf) {
          const llvm::DenseSet<int> &boxed =
              analysis_.unknownAnyFn ? analysis_.valueFns : analysis_.unknownFns;
          size_t known = analysis_.records.size();
          for (size_t k = 0; k < known; ++k) {
            const FnInfo &target = analysis_.records[k];
            if (boxed.contains(target.decl->id)) {
              analysis_.callWalk.addCallee(rec, target.decl, analysis_.classRefOrNone(target),
                                           true);
            }
          }
          continue;
        }
        if (rec.reflect != CallRec::Reflect::None) {
          std::vector<const ClassDecl *> classes = analysis_.programClasses;
          for (const ClassDecl *cls : analysis_.classes) {
            if (!cls->library() && !llvm::is_contained(classes, cls)) {
              classes.push_back(cls);
            }
          }
          for (const ClassDecl *cls : classes) {
            if (cls->generic()) {
              continue;
            }
            if (rec.reflect == CallRec::Reflect::Apply) {
              const Type *object = rec.byObject ? rec.byObject->nonOptional() : nullptr;
              if (object && object->isObject() && !object->isAnyObject() && cls->type &&
                  !analysis_.aliasAnalysis.typesMayAlias(cls->type, object)) {
                continue;
              }
              for (const FunctionDecl *method : cls->methods) {
                if (!method->isStatic() && (rec.reflectNames.empty() ||
                                            llvm::is_contained(rec.reflectNames, method->name))) {
                  analysis_.callWalk.addCallee(rec, method, rec.bySelf, true);
                }
              }
              continue;
            }
            bool hidden = !cls->constructors.empty() &&
                          llvm::all_of(cls->constructors, [](const FunctionDecl *ctor) {
                            return ctor->access() != tz::Access::Public;
                          });
            if (hidden) {
              continue;
            }
            std::optional<Ref> self = Ref{RefKind::Site, rec.site};
            for (const FunctionDecl *ctor : cls->constructors) {
              analysis_.callWalk.addCallee(rec, ctor, self, true);
            }
            for (const ClassDecl *k = cls; k; k = k->parent) {
              if (k->init) {
                analysis_.callWalk.addCallee(rec, k->init, self, true);
              }
            }
          }
          continue;
        }
        if (rec.channel >= 0) {
          for (int h : std::vector<int>(analysis_.listeners[rec.channel])) {
            const FnInfo &target = *analysis_.byFunction[h];
            analysis_.callWalk.addCallee(rec, target.decl, analysis_.classRefOrNone(target), true);
          }
          for (const Type *t : analysis_.listenerTypes[rec.channel]) {
            auto found = analysis_.values.find(t->nonOptional());
            if (found != analysis_.values.end()) {
              for (int g : std::vector<int>(found->second)) {
                const FnInfo &target = *analysis_.byFunction[g];
                analysis_.callWalk.addCallee(rec, target.decl, analysis_.classRefOrNone(target),
                                             true);
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
            for (int g : found->second) {
              const FnInfo &target = *analysis_.byFunction[g];
              analysis_.callWalk.addCallee(rec, target.decl, analysis_.classRefOrNone(target),
                                           true);
            }
          }
          continue;
        }
        auto found = analysis_.values.find(rec.byType->nonOptional());
        if (found != analysis_.values.end()) {
          for (int g : std::vector<int>(found->second)) {
            const FnInfo &target = *analysis_.byFunction[g];
            std::optional<Ref> self;
            if (target.thisOwner) {
              self = analysis_.callWalk.classRef(target.thisOwner);
            }
            analysis_.callWalk.addCallee(rec, target.decl, self, true);
          }
        }
        if (!rec.byName.empty() && rec.byObject) {
          const Type *object = rec.byObject->nonOptional();
          for (size_t k = 0; k < analysis_.classes.size(); ++k) {
            const ClassDecl *cls = analysis_.classes[k];
            if (!cls->type ||
                !(cls->literal() || analysis_.aliasAnalysis.typesMayAlias(cls->type, object))) {
              continue;
            }
            for (const FunctionDecl *method : cls->methods) {
              if (method->name == rec.byName && !method->isStatic()) {
                analysis_.callWalk.addCallee(rec, method, rec.bySelf, true);
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

std::optional<Access> CallResolver::substitute(const Access &a, const FnInfo &callee, int caller,
                                               const CallEdge &call, const CallRec &rec) const {
  const std::optional<Ref> &self = call.self;
  Access b = a;
  if (b.bound && callee.decl) {
    const std::vector<Param> &params = callee.decl->params;
    auto it = llvm::find_if(params, [&](const Param &p) { return p.sym == b.bound; });
    if (it != params.end()) {
      const Expr *arg = argument(rec, size_t(it - params.begin()));
      const FunctionDecl *passed = functionOf(arg);
      const VarSymbol *param = callableParam(analysis_.byFunction[caller]->decl, arg);
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
  bool byLibrary =
      rec.kind == CallKind::Inline && rec.call && analysis_.libraryEffects.libraryTarget(rec.call);
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
    std::optional<Ref> ref = analysis_.ownership.substituted(b.loc.ref, callee, caller, rec, call);
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
      b.loc.ref = analysis_.callWalk.classRef(owner);
    }
  }

  return b;
}

void CallResolver::fold() {
  for (FnInfo &f : analysis_.records) {
    for (unsigned s = 0; s < f.segs.size(); ++s) {
      if (f.isInline(s)) {
        f.inlineEffects.insert(f.segs[s].effects.begin(), f.segs[s].effects.end());
      }
    }
  }
  std::deque<int> queue;
  std::vector<bool> queued(analysis_.byFunction.size(), false);
  for (const FnInfo &f : analysis_.records) {
    queue.push_back(f.decl->id);
    queued[f.decl->id] = true;
  }
  while (!queue.empty()) {
    int fi = queue.front();
    queue.pop_front();
    queued[fi] = false;
    FnInfo &f = *analysis_.byFunction[fi];
    bool grew = false;
    for (int index : analysis_.callGraph.outgoing(fi)) {
      const CallEdge &call = analysis_.callGraph.edge(index);
      if (call.mode == Mode::Callback) {
        continue;
      }
      const CallRec &rec = f.calls[call.call];
      const FnInfo &g = *analysis_.byFunction[call.to];
      std::vector<Access> added;
      for (const Access &a : g.inlineEffects) {
        if (std::optional<Access> b = substitute(a, g, fi, call, rec)) {
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
    if (!grew) {
      continue;
    }
    for (int index : analysis_.callGraph.incoming(fi)) {
      const CallEdge &call = analysis_.callGraph.edge(index);
      if (call.mode != Mode::Callback && !queued[call.from]) {
        queued[call.from] = true;
        queue.push_back(call.from);
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
  const std::vector<Expr *> &args = n->ctor() ? n->ctor()->args : n->args;

  return index < args.size() ? args[index] : nullptr;
}

const FunctionDecl *CallResolver::functionOf(const Expr *e) {
  e = strip(e);
  if (auto *fe = dyn_cast_or_null<FunctionExpr>(e)) {
    return fe->fn;
  }
  auto *n = dyn_cast_or_null<NameExpr>(e);
  if (n && n->func()) {
    return n->func()->decl;
  }
  if (n && n->var() && !n->var()->reassigned && (n->var()->lambda() || n->var()->function())) {
    return n->var()->lambda() ? n->var()->lambda() : n->var()->function();
  }

  return nullptr;
}

const VarSymbol *CallResolver::callableParam(const FunctionDecl *fn, const Expr *e) {
  auto *n = dyn_cast_or_null<NameExpr>(strip(e));
  if (!fn || !n || !n->var() || n->var()->reassigned) {
    return nullptr;
  }
  bool param = llvm::any_of(fn->params, [&](const Param &p) { return p.sym == n->var(); });

  return param ? n->var() : nullptr;
}

std::optional<Ref> CallResolver::bind(const std::optional<Ref> &self,
                                      const Activation &caller) const {
  if (!self || self->kind != RefKind::This) {
    return self;
  }
  auto *owner = static_cast<const FunctionDecl *>(self->ptr);
  if (owner == analysis_.byFunction[caller.fn]->thisOwner && caller.self) {
    return caller.self;
  }

  return analysis_.callWalk.classRef(owner);
}

}
