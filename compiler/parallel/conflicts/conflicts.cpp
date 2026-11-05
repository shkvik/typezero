#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/analysis/analysis.h"
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
  const PieceInst &pa = analysis_.pieceInstances[a], &pb = analysis_.pieceInstances[b];
  if (pa.root == pb.root) {
    return analysis_.roots[pa.root].multi;
  }
  auto contains = [](const std::vector<unsigned> &v, unsigned x) {
    return std::binary_search(v.begin(), v.end(), x);
  };
  for (const auto &[r, interval] : analysis_.roots[pa.root].chain) {
    if (r == pb.root) {
      return analysis_.roots[r].multi || contains(interval, b);
    }
  }
  for (const auto &[r, interval] : analysis_.roots[pb.root].chain) {
    if (r == pa.root) {
      return analysis_.roots[r].multi || contains(interval, a);
    }
  }
  for (const auto &[ra, ia] : analysis_.roots[pa.root].chain) {
    for (const auto &[rb, ib] : analysis_.roots[pb.root].chain) {
      if (ra == rb) {
        if (analysis_.roots[ra].multi) {
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
  std::set<std::tuple<size_t, bool, const char *>> seen;
  for (const Access &a : effects) {
    auto [it, inserted] = index.try_emplace(a.loc, out.size());
    if (inserted) {
      out.emplace_back();
      out.back().loc = a.loc;
    }
    Touch &t = out[it->second];
    (a.write ? t.write : t.read) = true;
    if (seen.insert({it->second, a.write, a.at.getPointer()}).second) {
      auto at = std::make_pair(a.at, a.via);
      if (!llvm::is_contained(t.ats, at)) {
        t.ats.push_back(at);
      }
      auto &mine = a.write ? t.writeAts : t.readAts;
      if (!llvm::is_contained(mine, at)) {
        mine.push_back(at);
      }
    }
    bool inner = a.origin.isValid() && a.anchor.isValid() &&
                 !analysis_.partedAnchors.contains(a.anchor.getPointer());
    WaitPlace place{a.at, inner ? a.anchor : llvm::SMLoc(), inner ? a.home : nullptr};
    if (!llvm::is_contained(t.places, place)) {
      t.places.push_back(place);
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
    return loc.key == analysis_.star ? kAnyField | kFieldBits : hash(loc.key, 40, 24);
  case LocKind::Elements:
  case LocKind::OwnCell:
    return kElements;
  case LocKind::Contents:
    return kElements | kAnyField | kFieldBits;
  }

  return ~uint64_t(0);
}

void Conflicts::decide() {
  analysis_.pieceTouches.resize(analysis_.pieceInstances.size());
  for (unsigned i = 0; i < analysis_.pieceInstances.size(); ++i) {
    const PieceInst &p = analysis_.pieceInstances[i];
    const FnInfo &f = analysis_.fns[p.fn];
    Effects effects;
    for (const Access &a : f.segs[p.seg].effects) {
      if (!analysis_.pieces.keeps(p, a)) {
        continue;
      }
      Access b = a;
      if (b.loc.ref.kind == RefKind::This) {
        auto *owner = static_cast<const FunctionDecl *>(b.loc.ref.ptr);
        if (owner == f.thisOwner && p.self) {
          if (p.self->kind == RefKind::Local) {
            continue;
          }
          b.loc.ref = *p.self;
        } else {
          b.loc.ref = analysis_.callWalk.classRef(owner);
        }
      }
      effects.insert(b);
    }
    analysis_.pieceTouches[i] = touches(effects);
  }
  analysis_.decisions.assign(analysis_.pieceInstances.size(), {});
  std::vector<uint64_t> touched(analysis_.pieceInstances.size(), 0),
      written(analysis_.pieceInstances.size(), 0);
  for (unsigned i = 0; i < analysis_.pieceInstances.size(); ++i) {
    for (const Touch &t : analysis_.pieceTouches[i]) {
      uint64_t bits = maskOf(t.loc);
      touched[i] |= bits;
      if (t.write) {
        written[i] |= bits;
      }
    }
  }
  for (unsigned i = 0; i < analysis_.pieceInstances.size(); ++i) {
    for (unsigned j = i; j < analysis_.pieceInstances.size(); ++j) {
      bool maybe = (written[i] & touched[j]) || (written[j] & touched[i]);
      if (!maybe && !analysis_.decisions[i].alone && !analysis_.decisions[j].alone) {
        continue;
      }
      if (!concurrent(i, j)) {
        continue;
      }
      analysis_.decisions[i].alone = analysis_.decisions[j].alone = false;
      if (!maybe) {
        continue;
      }
      std::vector<Loc> shared;
      std::vector<const Touch *> mineI, mineJ;
      for (const Touch &x : analysis_.pieceTouches[i]) {
        for (const Touch &y : analysis_.pieceTouches[j]) {
          if (!(x.write || y.write)) {
            continue;
          }
          if (x.loc.kind == LocKind::OwnCell && y.loc.kind == LocKind::OwnCell &&
              x.loc.key == y.loc.key &&
              analysis_.pieceInstances[i].root == analysis_.pieceInstances[j].root &&
              analysis_.roots[analysis_.pieceInstances[i].root].ownCell == x.loc.key) {
            continue;
          }
          Loc lx = x.loc, ly = y.loc;
          if (lx.kind == LocKind::OwnCell) {
            lx.kind = LocKind::Elements;
          }
          if (ly.kind == LocKind::OwnCell) {
            ly.kind = LocKind::Elements;
          }
          if (!analysis_.aliasAnalysis.locsMayMeet(lx, ly)) {
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
      auto extend = [&](Decision &d, const std::vector<const Touch *> &mine) {
        for (const Touch *t : mine) {
          for (const auto &[at, via] : t->ats) {
            if (!d.from.isValid() ||
                analysis_.explainNames.posOf(at) < analysis_.explainNames.posOf(d.from)) {
              d.from = at;
            }
            if (!d.to.isValid() ||
                analysis_.explainNames.posOf(d.to) < analysis_.explainNames.posOf(at)) {
              d.to = at;
            }
            if (!llvm::is_contained(d.ats, at)) {
              d.ats.push_back(at);
            }
          }
          for (const WaitPlace &place : t->places) {
            if (!llvm::is_contained(d.places, place)) {
              d.places.push_back(place);
            }
          }
        }
      };
      analysis_.decisions[i].partners.push_back({j, shared});
      extend(analysis_.decisions[i], mineI);
      if (j != i) {
        std::vector<Loc> theirs;
        for (const Touch *t : mineJ) {
          if (!llvm::is_contained(theirs, t->loc)) {
            theirs.push_back(t->loc);
          }
        }
        analysis_.decisions[j].partners.push_back({i, theirs});
        extend(analysis_.decisions[j], mineJ);
      }
    }
  }
  placeWaits();
}

bool Conflicts::reachers(const FunctionDecl *home, std::vector<unsigned> &out) const {
  auto found = analysis_.fnIndex.find(home);
  if (found == analysis_.fnIndex.end()) {
    return false;
  }
  std::vector<bool> seen(analysis_.fns.size(), false);
  std::deque<unsigned> queue{found->second};
  seen[found->second] = true;
  auto add = [&](unsigned fn, unsigned seg) {
    auto it = analysis_.segPieces.find({fn, seg});
    if (it != analysis_.segPieces.end()) {
      out.insert(out.end(), it->second.begin(), it->second.end());
    }
  };
  const FnInfo &own = analysis_.fns[found->second];
  for (unsigned s = 0; s < own.segs.size(); ++s) {
    if (own.isInline(s)) {
      add(found->second, s);
    }
  }
  while (!queue.empty()) {
    unsigned g = queue.front();
    queue.pop_front();
    for (const auto &[caller, r] : analysis_.fns[g].callers) {
      const FnInfo &f = analysis_.fns[caller];
      for (unsigned s : f.calls[r].segs) {
        add(caller, s);
        if (f.isInline(s) && !seen[caller]) {
          seen[caller] = true;
          queue.push_back(caller);
        }
      }
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());

  return true;
}

void Conflicts::placeWaits() {
  std::vector<Decision> &decisions = analysis_.decisions;
  std::vector<llvm::DenseSet<const char *>> mine(decisions.size());
  std::map<const FunctionDecl *, std::set<const char *>> byHome;
  for (unsigned i = 0; i < decisions.size(); ++i) {
    for (const WaitPlace &place : decisions[i].places) {
      if (place.home) {
        mine[i].insert(place.anchor.getPointer());
        byHome[place.home].insert(place.anchor.getPointer());
      }
    }
  }
  llvm::DenseSet<const char *> inner;
  for (const auto &[home, points] : byHome) {
    std::vector<unsigned> reach;
    if (!reachers(home, reach)) {
      continue;
    }
    for (const char *anchor : points) {
      bool everywhere = true;
      for (unsigned r : reach) {
        if (!decisions[r].alone && !mine[r].contains(anchor)) {
          everywhere = false;
          break;
        }
      }
      if (everywhere) {
        inner.insert(anchor);
      }
    }
  }
  for (Decision &d : decisions) {
    for (const WaitPlace &place : d.places) {
      bool moved = place.home && inner.contains(place.anchor.getPointer());
      llvm::SMLoc wait = moved ? place.anchor : place.at;
      if (!llvm::is_contained(d.waits, wait)) {
        d.waits.push_back(wait);
      }
      auto where = std::make_pair(place.anchor, place.home);
      if (moved && !llvm::is_contained(d.inside, where)) {
        d.inside.push_back(where);
      }
    }
  }
}

}
