#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/explain_names/explain_names.h"
#include "compiler/parallel/explainer/explainer.h"
#include "compiler/parallel/expr_walk/expr_walk.h"
#include "compiler/parallel/library_effects/library_effects.h"
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

Conflicts::Conflicts(Analysis &analysis) : analysis_(analysis) {}

bool Conflicts::concurrent(unsigned a, unsigned b) const {
  const PieceInst &pa = analysis_.pieces_[a], &pb = analysis_.pieces_[b];
  if (pa.root == pb.root) {
    return analysis_.roots_[pa.root].multi;
  }
  auto contains = [](const std::vector<unsigned> &v, unsigned x) {
    return std::binary_search(v.begin(), v.end(), x);
  };
  for (const auto &[r, interval] : analysis_.roots_[pa.root].chain) {
    if (r == pb.root) {
      return analysis_.roots_[r].multi || contains(interval, b);
    }
  }
  for (const auto &[r, interval] : analysis_.roots_[pb.root].chain) {
    if (r == pa.root) {
      return analysis_.roots_[r].multi || contains(interval, a);
    }
  }
  for (const auto &[ra, ia] : analysis_.roots_[pa.root].chain) {
    for (const auto &[rb, ib] : analysis_.roots_[pb.root].chain) {
      if (ra == rb) {
        if (analysis_.roots_[ra].multi) {
          return true;
        }
        for (unsigned x : ia) {
          if (contains(ib, x)) {
            return true;
          }
        }
        return false;
      }
    }
  }

  return true;
}

std::vector<Touch> Conflicts::touches(const Effects &effects) const {
  std::vector<Touch> out;
  std::map<Loc, size_t> index;
  for (const Access &a : effects) {
    auto [it, inserted] = index.try_emplace(a.loc, out.size());
    if (inserted) {
      out.emplace_back();
      out.back().loc = a.loc;
    }
    Touch &t = out[it->second];
    (a.write ? t.write : t.read) = true;
    auto at = std::make_pair(a.at, a.via);
    if (!llvm::is_contained(t.ats, at)) {
      t.ats.push_back(at);
    }
    auto &mine = a.write ? t.writeAts : t.readAts;
    if (!llvm::is_contained(mine, at)) {
      mine.push_back(at);
    }
  }

  return out;
}

uint64_t Conflicts::maskOf(const Loc &loc) const {
  constexpr uint64_t kElements = uint64_t(1) << 8, kAnyField = uint64_t(1) << 9;
  constexpr uint64_t kFieldBits = ~uint64_t(0) << 40;
  auto hash = [](const void *p, unsigned first, unsigned count) {
    return uint64_t(1) << (first + unsigned((reinterpret_cast<uintptr_t>(p) >> 4) % count));
  };
  switch (loc.kind) {
  case LocKind::Resource:
    return uint64_t(1) << (loc.res == Res::Stderr ? unsigned(Res::Stdout) : unsigned(loc.res));
  case LocKind::Var:
    return hash(loc.key, 10, 30);
  case LocKind::Field:
    return loc.key == analysis_.star_ ? kAnyField | kFieldBits : hash(loc.key, 40, 24);
  case LocKind::Elements:
  case LocKind::OwnCell:
    return kElements;
  case LocKind::Contents:
    return kElements | kAnyField | kFieldBits;
  }

  return ~uint64_t(0);
}

void Conflicts::decide() {
  analysis_.pieceTouches_.resize(analysis_.pieces_.size());
  for (unsigned i = 0; i < analysis_.pieces_.size(); ++i) {
    const PieceInst &p = analysis_.pieces_[i];
    const FnInfo &f = analysis_.fns_[p.fn];
    Effects effects;
    for (const Access &a : f.segs[p.seg].effects) {
      Access b = a;
      if (b.loc.ref.kind == RefKind::This) {
        auto *owner = static_cast<const FunctionDecl *>(b.loc.ref.ptr);
        if (owner == f.thisOwner && p.self) {
          if (p.self->kind == RefKind::Local) {
            continue;
          }
          b.loc.ref = *p.self;
        } else {
          b.loc.ref = analysis_.callWalk().classRef(owner);
        }
      }
      effects.insert(b);
    }
    analysis_.pieceTouches_[i] = touches(effects);
  }
  analysis_.decisions_.assign(analysis_.pieces_.size(), {});
  std::vector<uint64_t> touched(analysis_.pieces_.size(), 0), written(analysis_.pieces_.size(), 0);
  for (unsigned i = 0; i < analysis_.pieces_.size(); ++i) {
    for (const Touch &t : analysis_.pieceTouches_[i]) {
      uint64_t bits = maskOf(t.loc);
      touched[i] |= bits;
      if (t.write) {
        written[i] |= bits;
      }
    }
  }
  for (unsigned i = 0; i < analysis_.pieces_.size(); ++i) {
    for (unsigned j = i; j < analysis_.pieces_.size(); ++j) {
      bool maybe = (written[i] & touched[j]) || (written[j] & touched[i]);
      if (!maybe && !analysis_.decisions_[i].alone && !analysis_.decisions_[j].alone) {
        continue;
      }
      if (!concurrent(i, j)) {
        continue;
      }
      analysis_.decisions_[i].alone = analysis_.decisions_[j].alone = false;
      if (!maybe) {
        continue;
      }
      std::vector<Loc> shared;
      std::vector<const Touch *> mineI, mineJ;
      for (const Touch &x : analysis_.pieceTouches_[i]) {
        for (const Touch &y : analysis_.pieceTouches_[j]) {
          if (!(x.write || y.write)) {
            continue;
          }
          if (x.loc.kind == LocKind::OwnCell && y.loc.kind == LocKind::OwnCell &&
              x.loc.key == y.loc.key && analysis_.pieces_[i].root == analysis_.pieces_[j].root &&
              analysis_.roots_[analysis_.pieces_[i].root].ownCell == x.loc.key) {
            continue;
          }
          Loc lx = x.loc, ly = y.loc;
          if (lx.kind == LocKind::OwnCell) {
            lx.kind = LocKind::Elements;
          }
          if (ly.kind == LocKind::OwnCell) {
            ly.kind = LocKind::Elements;
          }
          if (!analysis_.aliasAnalysis().locsMayMeet(lx, ly)) {
            continue;
          }
          if (!llvm::is_contained(shared, x.loc)) {
            shared.push_back(x.loc);
          }
          mineI.push_back(&x);
          mineJ.push_back(&y);
        }
      }
      if (shared.empty()) {
        continue;
      }
      auto extend = [&](Analysis::Decision &d, const std::vector<const Touch *> &mine) {
        for (const Touch *t : mine) {
          for (const auto &[at, via] : t->ats) {
            if (!d.from.isValid() ||
                analysis_.explainNames().posOf(at) < analysis_.explainNames().posOf(d.from)) {
              d.from = at;
            }
            if (!d.to.isValid() ||
                analysis_.explainNames().posOf(d.to) < analysis_.explainNames().posOf(at)) {
              d.to = at;
            }
            if (!llvm::is_contained(d.ats, at)) {
              d.ats.push_back(at);
            }
          }
        }
      };
      analysis_.decisions_[i].partners.push_back({j, shared});
      extend(analysis_.decisions_[i], mineI);
      if (j != i) {
        std::vector<Loc> theirs;
        for (const Touch *t : mineJ) {
          if (!llvm::is_contained(theirs, t->loc)) {
            theirs.push_back(t->loc);
          }
        }
        analysis_.decisions_[j].partners.push_back({i, theirs});
        extend(analysis_.decisions_[j], mineJ);
      }
    }
  }
}

}
