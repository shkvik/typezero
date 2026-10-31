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
    while (!analysis_.worklist_.empty()) {
      unsigned fi = analysis_.worklist_.front();
      analysis_.worklist_.pop_front();
      analysis_.codeWalk().walk(fi);
    }
    std::map<const Type *, std::vector<unsigned>> generators;
    for (unsigned fi = 0; fi < analysis_.fns_.size(); ++fi) {
      const FnInfo &f = analysis_.fns_[fi];
      if ((f.syncGen || f.asyncGen) && f.decl->sym && f.decl->sym->ret) {
        generators[f.decl->sym->ret].push_back(fi);
      }
    }
    for (unsigned fi = 0; fi < analysis_.fns_.size(); ++fi) {
      FnInfo &caller = analysis_.fns_[fi];
      for (CallRec &rec : caller.calls) {
        if (rec.reflect != CallRec::Reflect::None) {
          std::vector<const ClassDecl *> classes = analysis_.programClasses_;
          for (const ClassDecl *cls : analysis_.classes_) {
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
                if (!method->isStatic) {
                  analysis_.callWalk().addCallee(rec, method, rec.bySelf, true);
                }
              }
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
          for (unsigned h : std::vector<unsigned>(analysis_.listeners_[rec.channel])) {
            analysis_.callWalk().addCallee(rec, analysis_.fns_[h].decl,
                                           analysis_.classRefOrNone(analysis_.fns_[h]), true);
          }
          for (const Type *t : analysis_.listenerTypes_[rec.channel]) {
            auto found = analysis_.values_.find(t->nonOptional());
            if (found != analysis_.values_.end()) {
              for (unsigned g : std::vector<unsigned>(found->second)) {
                analysis_.callWalk().addCallee(rec, analysis_.fns_[g].decl,
                                               analysis_.classRefOrNone(analysis_.fns_[g]), true);
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
              analysis_.callWalk().addCallee(rec, analysis_.fns_[g].decl,
                                             analysis_.classRefOrNone(analysis_.fns_[g]), true);
            }
          }
          continue;
        }
        auto found = analysis_.values_.find(rec.byType->nonOptional());
        if (found != analysis_.values_.end()) {
          for (unsigned g : std::vector<unsigned>(found->second)) {
            std::optional<Ref> self;
            if (analysis_.fns_[g].thisOwner) {
              self = analysis_.callWalk().classRef(analysis_.fns_[g].thisOwner);
            }
            analysis_.callWalk().addCallee(rec, analysis_.fns_[g].decl, self, true);
          }
        }
        if (!rec.byName.empty() && rec.byObject) {
          const Type *object = rec.byObject->nonOptional();
          for (size_t k = 0; k < analysis_.classes_.size(); ++k) {
            const ClassDecl *cls = analysis_.classes_[k];
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
    if (analysis_.worklist_.empty()) {
      break;
    }
  }
}

std::optional<Access> CallResolver::substitute(const Access &a, const FnInfo &callee,
                                               const FnInfo &caller, const Callee &call,
                                               const CallRec &rec) const {
  const std::optional<Ref> &self = call.self;
  Access b = a;
  b.at = rec.loc;
  b.via = callee.decl;
  if (b.loc.ref.kind == RefKind::Owned) {
    b.loc.ref = analysis_.ownership().substituted(b.loc.ref, callee, caller, rec, call.mode);
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
  for (unsigned fi = 0; fi < analysis_.fns_.size(); ++fi) {
    FnInfo &f = analysis_.fns_[fi];
    for (unsigned s = 0; s < f.segs.size(); ++s) {
      if (f.isInline(s)) {
        f.inlineEffects.insert(f.segs[s].effects.begin(), f.segs[s].effects.end());
      }
    }
    for (unsigned r = 0; r < f.calls.size(); ++r) {
      for (const Callee &c : f.calls[r].callees) {
        analysis_.fns_[c.fn].named.push_back({fi, r});
        if (c.mode != Mode::Callback) {
          analysis_.fns_[c.fn].callers.push_back({fi, r});
        }
      }
    }
  }
  std::deque<unsigned> queue;
  std::vector<bool> queued(analysis_.fns_.size(), true);
  for (unsigned fi = 0; fi < analysis_.fns_.size(); ++fi) {
    queue.push_back(fi);
  }
  while (!queue.empty()) {
    unsigned fi = queue.front();
    queue.pop_front();
    queued[fi] = false;
    FnInfo &f = analysis_.fns_[fi];
    bool grew = false;
    for (const CallRec &rec : f.calls) {
      for (const Callee &c : rec.callees) {
        if (c.mode == Mode::Callback) {
          continue;
        }
        const FnInfo &g = analysis_.fns_[c.fn];
        std::vector<Access> added;
        for (const Access &a : g.inlineEffects) {
          if (std::optional<Access> b = substitute(a, g, f, c, rec)) {
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

std::optional<Ref> CallResolver::bind(const std::optional<Ref> &self,
                                      const Activation &caller) const {
  if (!self || self->kind != RefKind::This) {
    return self;
  }
  auto *owner = static_cast<const FunctionDecl *>(self->ptr);
  if (owner == analysis_.fns_[caller.fn].thisOwner && caller.self) {
    return caller.self;
  }

  return analysis_.callWalk().classRef(owner);
}

}
