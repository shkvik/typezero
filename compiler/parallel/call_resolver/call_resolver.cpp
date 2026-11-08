#include "compiler/parallel/call_resolver/call_resolver.h"
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
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
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
    : analysis(analysis) {}

void CallResolver::resolve() {
  while (true) {
    while (!this->analysis.worklist.empty()) {
      int fi = this->analysis.worklist.front();
      this->analysis.worklist.pop_front();
      this->analysis.codeWalk.walk(fi);
    }
    std::map<const Type *, std::vector<int>> generators;
    for (size_t i = 0; i < this->analysis.records.size(); ++i) {
      const FnInfo &f = this->analysis.records[i];
      if ((f.syncGen || f.asyncGen) && f.decl->sym && f.decl->sym->hasReturnType()) {
        generators[f.decl->sym->getKnownReturnType()].push_back(f.decl->id);
      }
    }
    for (size_t i = 0; i < this->analysis.records.size(); ++i) {
      FnInfo &caller = this->analysis.records[i];
      for (CallRec &rec : caller.calls) {
        if (rec.reflect == CallRec::Reflect::Apply && rec.noSelf) {
          const llvm::DenseSet<int> &boxed =
              this->analysis.unknownAnyFn ? this->analysis.valueFns : this->analysis.unknownFns;
          size_t known = this->analysis.records.size();
          for (size_t k = 0; k < known; ++k) {
            const FnInfo &target = this->analysis.records[k];
            if (boxed.contains(target.decl->id)) {
              this->analysis.callWalk.addCallee(rec, target.decl,
                                                this->analysis.classRefOrNone(target), true);
            }
          }
          continue;
        }
        if (rec.reflect != CallRec::Reflect::None) {
          std::vector<const ClassDecl *> classes = this->analysis.programClasses;
          for (const ClassDecl *cls : this->analysis.classes) {
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
                  !this->analysis.aliasAnalysis.typesMayAlias(cls->type, object)) {
                continue;
              }
              for (const FunctionDecl *method : cls->methods) {
                if (!method->isStatic() && (rec.reflectNames.empty() ||
                                            llvm::is_contained(rec.reflectNames, method->name))) {
                  this->analysis.callWalk.addCallee(rec, method, rec.bySelf, true);
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
              this->analysis.callWalk.addCallee(rec, ctor, self, true);
            }
            for (const ClassDecl *k = cls; k; k = k->parent) {
              if (k->init) {
                this->analysis.callWalk.addCallee(rec, k->init, self, true);
              }
            }
          }
          continue;
        }
        if (rec.channel >= 0) {
          for (int h : std::vector<int>(this->analysis.listeners[rec.channel])) {
            const FnInfo &target = *this->analysis.byFunction[h];
            this->analysis.callWalk.addCallee(rec, target.decl,
                                              this->analysis.classRefOrNone(target), true);
          }
          for (const Type *t : this->analysis.listenerTypes[rec.channel]) {
            auto found = this->analysis.values.find(t->nonOptional());
            if (found != this->analysis.values.end()) {
              for (int g : std::vector<int>(found->second)) {
                const FnInfo &target = *this->analysis.byFunction[g];
                this->analysis.callWalk.addCallee(rec, target.decl,
                                                  this->analysis.classRefOrNone(target), true);
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
              const FnInfo &target = *this->analysis.byFunction[g];
              this->analysis.callWalk.addCallee(rec, target.decl,
                                                this->analysis.classRefOrNone(target), true);
            }
          }
          continue;
        }
        auto found = this->analysis.values.find(rec.byType->nonOptional());
        if (found != this->analysis.values.end()) {
          for (int g : std::vector<int>(found->second)) {
            const FnInfo &target = *this->analysis.byFunction[g];
            std::optional<Ref> self;
            if (target.thisOwner) {
              self = this->analysis.callWalk.classRef(target.thisOwner);
            }
            this->analysis.callWalk.addCallee(rec, target.decl, self, true);
          }
        }
        if (!rec.byName.empty() && rec.byObject) {
          const Type *object = rec.byObject->nonOptional();
          for (size_t k = 0; k < this->analysis.classes.size(); ++k) {
            const ClassDecl *cls = this->analysis.classes[k];
            if (!cls->type || !(cls->literal() ||
                                this->analysis.aliasAnalysis.typesMayAlias(cls->type, object))) {
              continue;
            }
            for (const FunctionDecl *method : cls->methods) {
              if (method->name == rec.byName && !method->isStatic()) {
                this->analysis.callWalk.addCallee(rec, method, rec.bySelf, true);
              }
            }
          }
        }
      }
    }
    if (this->analysis.worklist.empty()) {
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
      const VarSymbol *param = callableParam(this->analysis.byFunction[caller]->decl, arg);
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
                   this->analysis.libraryEffects.libraryTarget(rec.call);
  if (this->analysis.isLibrary(callee.decl) || byLibrary) {
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
        this->analysis.ownership.substituted(b.loc.ref, callee, caller, rec, call);
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
      b.loc.ref = this->analysis.callWalk.classRef(owner);
    }
  }

  return b;
}

void CallResolver::fold() {
  for (FnInfo &f : this->analysis.records) {
    for (unsigned s = 0; s < f.segs.size(); ++s) {
      if (f.isInline(s)) {
        f.inlineEffects.insert(f.segs[s].effects.begin(), f.segs[s].effects.end());
      }
    }
  }
  std::deque<int> queue;
  std::vector<bool> queued(this->analysis.byFunction.size(), false);
  for (const FnInfo &f : this->analysis.records) {
    queue.push_back(f.decl->id);
    queued[f.decl->id] = true;
  }
  while (!queue.empty()) {
    int fi = queue.front();
    queue.pop_front();
    queued[fi] = false;
    FnInfo &f = *this->analysis.byFunction[fi];
    bool grew = false;
    for (int index : this->analysis.callGraph.outgoing(fi)) {
      const CallEdge &call = this->analysis.callGraph.edge(index);
      if (call.mode == Mode::Callback) {
        continue;
      }
      const CallRec &rec = f.calls[call.call];
      const FnInfo &g = *this->analysis.byFunction[call.to];
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
    for (int index : this->analysis.callGraph.incoming(fi)) {
      const CallEdge &call = this->analysis.callGraph.edge(index);
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
  e = ParallelHelpers::strip(e);
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
  auto *n = dyn_cast_or_null<NameExpr>(ParallelHelpers::strip(e));
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
  if (owner == this->analysis.byFunction[caller.fn]->thisOwner && caller.self) {
    return caller.self;
  }

  return this->analysis.callWalk.classRef(owner);
}

}
