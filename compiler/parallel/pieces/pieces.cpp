#include "compiler/parallel/pieces/pieces.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/parallel/explain_names/explain_names.h"
#include "compiler/parallel/explainer/explainer.h"
#include "compiler/parallel/expr_walk/expr_walk.h"
#include "compiler/parallel/library_effects/library_effects.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
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

Pieces::Pieces(Analysis &analysis) : analysis_(analysis) {}

bool Pieces::scheduled(const Activation &act, unsigned seg) const {
  const FnInfo &f = analysis_.fns[act.fn];
  if (seg == 0) {
    return act.callbackStart;
  }

  return f.segs[seg].kind == SegKind::Await || f.segs[seg].kind == SegKind::ForAwait;
}

unsigned Pieces::activate(unsigned fn, int root, std::optional<Ref> self, bool callbackStart,
                          bool rootStart) {
  auto key = std::make_tuple(fn, root, self, callbackStart);
  auto [it, inserted] = analysis_.actIndex.try_emplace(key, unsigned(analysis_.acts.size()));
  if (inserted) {
    Activation &act = analysis_.acts.emplace_back();
    act.fn = fn;
    act.root = root;
    act.self = self;
    act.callbackStart = callbackStart;
    analysis_.actsByTask[{fn, root}].push_back(it->second);
  }
  analysis_.acts[it->second].rootStart |= rootStart;

  return it->second;
}

int Pieces::newRoot(int parent, unsigned fn, std::optional<Ref> self, unsigned callerFn,
                    unsigned rec, Mode mode) {
  for (int p = parent; p >= 0; p = analysis_.roots[p].parent) {
    Root &up = analysis_.roots[p];
    if (up.fn == fn && up.self == self && up.callerFn == callerFn && up.rec == int(rec)) {
      up.multi = up.recursive = true;
      return p;
    }
  }
  auto key = std::make_tuple(parent, fn, self, callerFn, rec);
  if (auto found = analysis_.rootIndex.find(key); found != analysis_.rootIndex.end()) {
    return found->second;
  }
  constexpr size_t kMaxRoots = 4096;
  if (analysis_.roots.size() >= kMaxRoots) {
    for (size_t i = 0; i < analysis_.roots.size(); ++i) {
      Root &r = analysis_.roots[i];
      if (r.fn == fn && r.callerFn == callerFn && r.rec == int(rec)) {
        r.multi = true;
        r.parent = -1;
        return int(i);
      }
    }
  }
  analysis_.rootIndex[key] = int(analysis_.roots.size());
  Root &root = analysis_.roots.emplace_back();
  root.fn = fn;
  root.self = self;
  root.parent = parent;
  root.callerFn = callerFn;
  root.rec = int(rec);
  root.mode = mode;

  return int(analysis_.roots.size() - 1);
}

void Pieces::explore() {
  Root &main = analysis_.roots.emplace_back();
  main.fn = 0;
  std::deque<unsigned> queue{activate(0, 0, std::nullopt, true, false)};
  llvm::DenseSet<unsigned> done;
  while (!queue.empty()) {
    unsigned ai = queue.front();
    queue.pop_front();
    if (!done.insert(ai).second) {
      continue;
    }
    Activation act = analysis_.acts[ai];
    const FnInfo &f = analysis_.fns[act.fn];
    for (unsigned r = 0; r < f.calls.size(); ++r) {
      const CallRec &rec = f.calls[r];
      for (const Callee &c : rec.callees) {
        std::optional<Ref> self = analysis_.callResolver().bind(c.self, act);
        unsigned next;
        switch (c.mode) {
        case Mode::Sync:
        case Mode::Await:
        case Mode::AsyncIter:
          next = activate(c.fn, act.root, self, false, false);
          analysis_.inlineEdges.push_back({ai, r, next});
          break;
        case Mode::Spawn:
          next = activate(c.fn, newRoot(act.root, c.fn, self, act.fn, r, Mode::Spawn), self, false,
                          true);
          break;
        case Mode::Callback:
          next = activate(c.fn, newRoot(act.root, c.fn, self, act.fn, r, Mode::Callback), self,
                          true, false);
          break;
        }
        analysis_.edges.push_back({ai, r, next});
        if (!done.contains(next)) {
          queue.push_back(next);
        }
      }
    }
  }
}

void Pieces::findBindings() {
  auto &bindings = analysis_.bindings;
  bindings.assign(analysis_.acts.size(), {});
  std::vector<bool> reached(analysis_.acts.size(), false);
  for (const auto &[from, r, to] : analysis_.edges) {
    reached[to] = true;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[from, r, to] : analysis_.edges) {
      const FunctionDecl *decl = analysis_.fns[analysis_.acts[to].fn].decl;
      const FunctionDecl *caller = analysis_.fns[analysis_.acts[from].fn].decl;
      if (!decl) {
        continue;
      }
      const CallRec &rec = analysis_.fns[analysis_.acts[from].fn].calls[r];
      for (size_t i = 0; i < decl->params.size(); ++i) {
        const VarSymbol *param = decl->params[i].sym;
        if (!param || !param->type || !param->type->nonOptional()->isFunction()) {
          continue;
        }
        std::set<const FunctionDecl *> add;
        const Expr *arg = CallResolver::argument(rec, i);
        if (const FunctionDecl *passed = CallResolver::functionOf(arg)) {
          add.insert(passed);
        } else if (const VarSymbol *outer = CallResolver::callableParam(caller, arg)) {
          if (!reached[from]) {
            add.insert(nullptr);
          } else if (auto found = bindings[from].find(outer); found != bindings[from].end()) {
            add = found->second;
          }
        } else {
          add.insert(nullptr);
        }
        std::set<const FunctionDecl *> &have = bindings[to][param];
        for (const FunctionDecl *fn : add) {
          changed |= have.insert(fn).second;
        }
      }
    }
  }
  analysis_.reached = std::move(reached);
}

bool Pieces::keeps(const PieceInst &piece, const Access &a) const {
  const FunctionDecl *decl = analysis_.fns[piece.fn].decl;
  if (!a.bound || !decl ||
      llvm::none_of(decl->params, [&](const Param &p) { return p.sym == a.bound; })) {
    return true;
  }
  for (unsigned ai : analysis_.actsOf(piece.fn, piece.root)) {
    const Activation &act = analysis_.acts[ai];
    if (act.self != piece.self) {
      continue;
    }
    if (!analysis_.reached[ai]) {
      return true;
    }
    auto found = analysis_.bindings[ai].find(a.bound);
    if (found != analysis_.bindings[ai].end() &&
        (found->second.count(nullptr) || found->second.count(a.through))) {
      return true;
    }
  }

  return false;
}

void Pieces::findPieces() {
  analysis_.actPieces.assign(analysis_.acts.size(), {});
  for (unsigned ai = 0; ai < analysis_.acts.size(); ++ai) {
    const Activation &act = analysis_.acts[ai];
    const FnInfo &f = analysis_.fns[act.fn];
    for (unsigned s = 0; s < f.segs.size(); ++s) {
      if (!scheduled(act, s)) {
        continue;
      }
      auto key = std::make_tuple(act.fn, s, act.root, act.self);
      auto [it, inserted] =
          analysis_.pieceIndex.try_emplace(key, unsigned(analysis_.pieceInstances.size()));
      if (inserted) {
        analysis_.pieceInstances.push_back({act.fn, s, act.root, act.self});
        analysis_.segPieces[{act.fn, s}].push_back(it->second);
        ++analysis_.sameTask[std::make_tuple(act.fn, s, act.root)];
      }
      analysis_.actPieces[ai].push_back(it->second);
    }
  }
}

bool Pieces::ownPiece(unsigned act, unsigned piece) const {
  return piece < analysis_.pieceInstances.size() &&
         llvm::is_contained(analysis_.actPieces[act], piece);
}

void Pieces::addEntry(unsigned act, unsigned piece, const Analysis::Entry &add, bool &changed) {
  auto [it, inserted] = analysis_.entries.try_emplace({act, piece}, add);
  if (inserted) {
    changed = true;
    return;
  }
  Analysis::Entry &cur = it->second;
  if (cur.open) {
    return;
  }
  if (add.open) {
    cur.open = changed = true;
    return;
  }
  const ExplainNames &names = analysis_.explainNames();
  if (names.posOf(add.first) < names.posOf(cur.first)) {
    cur.first = add.first;
    changed = true;
  }
  if (names.posOf(cur.last) < names.posOf(add.last)) {
    cur.last = add.last;
    changed = true;
  }
}

void Pieces::findHosts() {
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[caller, r, callee] : analysis_.inlineEdges) {
      const Activation &from = analysis_.acts[caller];
      const CallRec &rec = analysis_.fns[from.fn].calls[r];
      std::vector<unsigned> at = piecesAt(from, rec.segs);
      Activation &to = analysis_.acts[callee];
      for (unsigned p : at) {
        changed |= to.hosts.insert(p).second;
        Analysis::Entry add;
        if (ownPiece(caller, p)) {
          add.first = add.last = rec.loc;
        } else if (auto e = analysis_.entries.find({caller, p}); e != analysis_.entries.end()) {
          add = e->second;
        } else {
          add.open = true;
        }
        addEntry(callee, p, add, changed);
      }
    }
  }
}

int Pieces::stepOf(unsigned fn, llvm::SMLoc loc) {
  if (!loc.isValid()) {
    return -1;
  }
  auto [it, inserted] = analysis_.steps.try_emplace(fn);
  std::vector<Analysis::Pos> &steps = it->second;
  const ExplainNames &names = analysis_.explainNames();
  if (inserted) {
    const FunctionDecl *decl = analysis_.fns[fn].decl;
    if (decl && decl->body) {
      for (const Stmt *s : decl->body->body) {
        steps.push_back(names.posOf(s->loc()));
      }
    } else if (!decl) {
      for (const Stmt *s : analysis_.program.items) {
        if (!isa<FunctionDecl>(s) && !analysis_.libraryEffects().inLibrary(s->loc())) {
          steps.push_back(names.posOf(s->loc()));
        }
      }
    }
    llvm::sort(steps);
  }
  Analysis::Pos at = names.posOf(loc);
  auto next = std::upper_bound(steps.begin(), steps.end(), at);
  if (next == steps.begin() || std::prev(next)->buffer != at.buffer) {
    return -1;
  }

  return int(next - steps.begin()) - 1;
}

bool Pieces::later(unsigned fn, llvm::SMLoc at, llvm::SMLoc origin) {
  int from = stepOf(fn, at);
  int to = stepOf(fn, origin);

  return from < 0 || to < 0 || to >= from;
}

void Pieces::noteFlow(int root, unsigned from, unsigned to, llvm::SMLoc origin) {
  auto &origins = analysis_.flowFrom[root];
  auto [it, inserted] = origins.try_emplace({from, to}, origin);
  if (inserted || !it->second.isValid()) {
    return;
  }
  if (!origin.isValid()) {
    it->second = origin;
    return;
  }
  const ExplainNames &names = analysis_.explainNames();
  if (names.posOf(it->second) < names.posOf(origin)) {
    it->second = origin;
  }
}

std::vector<unsigned> Pieces::piecesAt(const Activation &act, llvm::ArrayRef<unsigned> segs) const {
  std::vector<unsigned> out;
  const FnInfo &f = analysis_.fns[act.fn];
  for (unsigned s : segs) {
    if (scheduled(act, s)) {
      auto it = analysis_.pieceIndex.find(std::make_tuple(act.fn, s, act.root, act.self));
      if (it != analysis_.pieceIndex.end()) {
        out.push_back(it->second);
      }
    } else if (f.isInline(s)) {
      if (act.rootStart) {
        out.push_back(analysis_.startNode(act.root));
      } else {
        out.insert(out.end(), act.hosts.begin(), act.hosts.end());
      }
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());

  return out;
}

bool Pieces::once(unsigned fi, unsigned rec, unsigned rootFn, unsigned depth) {
  if (analysis_.fns[fi].calls[rec].repeated) {
    return false;
  }

  return fnOnce(fi, rootFn, depth);
}

bool Pieces::fnOnce(unsigned fi, unsigned rootFn, unsigned depth) {
  if (fi == rootFn) {
    return true;
  }
  if (depth > 32 || analysis_.valueFns.contains(fi)) {
    return false;
  }
  auto key = std::make_pair(fi, rootFn);
  auto found = analysis_.fnOnce.find(key);
  if (found != analysis_.fnOnce.end()) {
    return found->second;
  }
  analysis_.fnOnce[key] = false;
  const FnInfo &f = analysis_.fns[fi];
  bool result = false;
  if (f.named.size() == 1) {
    auto [cf, cr] = f.named[0];
    const CallRec &rec = analysis_.fns[cf].calls[cr];
    bool direct = llvm::any_of(rec.callees, [&](const Callee &c) {
      return c.fn == fi && !c.byType &&
             (c.mode == Mode::Sync || c.mode == Mode::Await || c.mode == Mode::AsyncIter);
    });
    result = direct && once(cf, cr, rootFn, depth + 1);
  }

  return analysis_.fnOnce[key] = result;
}

bool Pieces::rejectionEnds(const AwaitExpr *a, unsigned depth) {
  auto found = analysis_.awaits.find(a);
  if (found == analysis_.awaits.end()) {
    return false;
  }

  return fnRejectionEnds(found->second.fn, depth);
}

bool Pieces::fnRejectionEnds(unsigned fi, unsigned depth) {
  if (fi == 0) {
    return true;
  }
  if (depth > 32 || analysis_.valueFns.contains(fi) || !analysis_.fns[fi].async) {
    return false;
  }
  auto found = analysis_.rejectionEnds.find(fi);
  if (found != analysis_.rejectionEnds.end()) {
    return found->second;
  }
  analysis_.rejectionEnds[fi] = false;
  bool result = true;
  for (const auto &[cf, cr] : analysis_.fns[fi].named) {
    const CallRec &rec = analysis_.fns[cf].calls[cr];
    for (const Callee &c : rec.callees) {
      if (c.fn != fi) {
        continue;
      }
      bool ends = false;
      switch (c.mode) {
      case Mode::Await:
        ends = rec.await ? rejectionEnds(rec.await, depth + 1)
                         : rec.returned && fnRejectionEnds(cf, depth + 1);
        break;
      case Mode::Spawn:
        ends = rec.voided || (rec.join && rejectionEnds(rec.join, depth + 1));
        break;
      case Mode::Callback:
        ends = !rec.handled;
        break;
      default:
        break;
      }
      result &= ends && !c.byType;
    }
  }

  return analysis_.rejectionEnds[fi] = result;
}

void Pieces::findJoins() {
  for (size_t i = 1; i < analysis_.roots.size(); ++i) {
    Root &root = analysis_.roots[i];
    if (root.rec < 0) {
      continue;
    }
    const FnInfo &caller = analysis_.fns[root.callerFn];
    const CallRec &rec = caller.calls[root.rec];
    int parentFn = root.parent >= 0 ? int(analysis_.roots[root.parent].fn) : -1;
    root.multi |= rec.interval || parentFn < 0 || !once(root.callerFn, root.rec, parentFn, 0) ||
                  (size_t(root.parent) < i && analysis_.roots[root.parent].multi);
    const FunctionDecl *callee = analysis_.fns[root.fn].decl;
    if (rec.kind == CallKind::Inline && rec.call && rec.call->builtin == Builtin::ArrMap &&
        callee && callee->params.size() > 1 && callee->params[1].sym &&
        analysis_.ownIndex.contains(callee->params[1].sym) && !rec.inLoop && parentFn >= 0 &&
        fnOnce(root.callerFn, unsigned(parentFn), 0)) {
      bool single = true;
      for (int p = root.parent; p >= 0; p = analysis_.roots[p].parent) {
        single &= !analysis_.roots[p].multi;
      }
      if (single) {
        root.ownCell = callee;
      }
    }
    if (root.parent < 0) {
      continue;
    }
    std::set<unsigned> spawnAt, interval;
    std::map<unsigned, llvm::SMLoc> spawnFrom;
    for (unsigned ai : analysis_.actsOf(root.callerFn, root.parent)) {
      for (unsigned p : piecesAt(analysis_.acts[ai], rec.segs)) {
        spawnAt.insert(p);
        llvm::SMLoc at;
        if (ownPiece(ai, p)) {
          at = rec.loc;
        } else if (auto e = analysis_.entries.find({ai, p});
                   e != analysis_.entries.end() && !e->second.open) {
          at = e->second.first;
        }
        auto [it, inserted] = spawnFrom.try_emplace(p, at);
        if (inserted || !it->second.isValid()) {
          continue;
        }
        if (!at.isValid() ||
            analysis_.explainNames().posOf(at) < analysis_.explainNames().posOf(it->second)) {
          it->second = at;
        }
      }
    }
    root.spawnAt.assign(spawnAt.begin(), spawnAt.end());
    root.spawnFrom.assign(spawnFrom.begin(), spawnFrom.end());
    if (root.mode != Mode::Spawn || !rec.join) {
      continue;
    }
    auto join = analysis_.joins.find(rec.join);
    if (join == analysis_.joins.end() || !rejectionEnds(rec.join, 0)) {
      continue;
    }
    bool found = false;
    for (unsigned ai : analysis_.actsOf(join->second.fn, root.parent)) {
      found = true;
      for (unsigned p : piecesAt(analysis_.acts[ai], join->second.segs)) {
        interval.insert(p);
      }
    }
    if (!found) {
      continue;
    }
    root.join = rec.join;
    root.interval.assign(interval.begin(), interval.end());
  }
  for (size_t i = 1; i < analysis_.roots.size(); ++i) {
    Root &root = analysis_.roots[i];
    if (root.parent < 0 || size_t(root.parent) >= i) {
      continue;
    }
    const Root &parent = analysis_.roots[root.parent];
    if (root.join) {
      root.chain.push_back({root.parent, root.interval});
      root.chain.insert(root.chain.end(), parent.chain.begin(), parent.chain.end());
      continue;
    }
    root.chain.push_back({root.parent, reachAfter(root.parent, root.spawnFrom)});
    for (const auto &[up, pieces] : parent.chain) {
      root.chain.push_back({up, reach(up, pieces)});
    }
  }
}

void Pieces::buildFlow() {
  analysis_.flow.assign(analysis_.roots.size(), {});
  analysis_.flowFrom.assign(analysis_.roots.size(), {});
  for (unsigned ai = 0; ai < analysis_.acts.size(); ++ai) {
    const Activation &act = analysis_.acts[ai];
    const FnInfo &f = analysis_.fns[act.fn];
    for (const auto &[a, b] : f.succ) {
      for (unsigned x : piecesAt(act, {a})) {
        llvm::SMLoc origin;
        if (scheduled(act, a)) {
          origin = f.segs[b].loc;
        } else if (auto e = analysis_.entries.find({ai, x});
                   e != analysis_.entries.end() && !e->second.open) {
          origin = e->second.last;
        }
        for (unsigned y : piecesAt(act, {b})) {
          analysis_.flow[act.root][x].push_back(y);
          noteFlow(act.root, x, y, origin);
        }
      }
    }
  }
  for (const auto &[caller, r, callee] : analysis_.inlineEdges) {
    const Activation &x = analysis_.acts[caller];
    if (analysis_.acts[callee].root != x.root || analysis_.actPieces[callee].empty()) {
      continue;
    }
    const FnInfo &f = analysis_.fns[x.fn];
    const CallRec &rec = f.calls[r];
    std::vector<unsigned> after;
    for (const auto &[a, b] : f.succ) {
      if (llvm::is_contained(rec.segs, a)) {
        for (unsigned n : piecesAt(x, {b})) {
          after.push_back(n);
        }
      }
    }
    for (unsigned p : analysis_.actPieces[callee]) {
      std::vector<unsigned> &next = analysis_.flow[x.root][p];
      next.insert(next.end(), after.begin(), after.end());
      for (unsigned n : after) {
        noteFlow(x.root, p, n, llvm::SMLoc());
      }
    }
  }
  for (auto &graph : analysis_.flow) {
    for (auto &[node, next] : graph) {
      std::sort(next.begin(), next.end());
      next.erase(std::unique(next.begin(), next.end()), next.end());
    }
  }
}

std::vector<unsigned> Pieces::reach(int root, const std::vector<unsigned> &from) const {
  std::set<unsigned> seen(from.begin(), from.end());
  std::vector<unsigned> pending(from.begin(), from.end());
  const auto &graph = analysis_.flow[root];
  while (!pending.empty()) {
    unsigned n = pending.back();
    pending.pop_back();
    auto found = graph.find(n);
    if (found == graph.end()) {
      continue;
    }
    for (unsigned m : found->second) {
      if (seen.insert(m).second) {
        pending.push_back(m);
      }
    }
  }

  return std::vector<unsigned>(seen.begin(), seen.end());
}

std::vector<unsigned>
Pieces::reachAfter(int root, const std::vector<std::pair<unsigned, llvm::SMLoc>> &from) {
  const auto &graph = analysis_.flow[root];
  const auto &origins = analysis_.flowFrom[root];
  std::set<unsigned> seen, expanded;
  std::vector<unsigned> pending;
  auto visit = [&](unsigned n) {
    seen.insert(n);
    if (expanded.insert(n).second) {
      pending.push_back(n);
    }
  };
  for (const auto &[p, at] : from) {
    seen.insert(p);
  }
  for (const auto &[p, at] : from) {
    auto found = graph.find(p);
    if (found == graph.end()) {
      continue;
    }
    for (unsigned n : found->second) {
      auto origin = origins.find({p, n});
      bool after = !at.isValid() || origin == origins.end() || !origin->second.isValid() ||
                   p >= analysis_.pieceInstances.size() ||
                   later(analysis_.pieceInstances[p].fn, at, origin->second);
      if (after) {
        visit(n);
      }
    }
  }
  while (!pending.empty()) {
    unsigned n = pending.back();
    pending.pop_back();
    auto found = graph.find(n);
    if (found == graph.end()) {
      continue;
    }
    for (unsigned m : found->second) {
      visit(m);
    }
  }

  return std::vector<unsigned>(seen.begin(), seen.end());
}
}
