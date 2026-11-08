#include "compiler/parallel/pieces/pieces.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/analysis/analysis.h"
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

Pieces::Pieces(Analysis &analysis)
    : analysis(analysis) {}

bool Pieces::scheduled(const Activation &act, unsigned seg) const {
  const FnInfo &f = *this->analysis.byFunction[act.fn];
  if (seg == 0) {
    return act.callbackStart;
  }

  return f.segs[seg].kind == SegKind::Await || f.segs[seg].kind == SegKind::ForAwait;
}

unsigned Pieces::activate(int fn, int root, std::optional<Ref> self, bool callbackStart,
                          bool rootStart) {
  auto key = std::make_tuple(fn, root, self, callbackStart);
  auto [it, inserted] =
      this->analysis.actIndex.try_emplace(key, unsigned(this->analysis.acts.size()));
  if (inserted) {
    Activation &act = this->analysis.acts.emplace_back();
    act.fn = fn;
    act.root = root;
    act.self = self;
    act.callbackStart = callbackStart;
    this->analysis.actsByTask[{fn, root}].push_back(it->second);
  }
  this->analysis.acts[it->second].rootStart |= rootStart;

  return it->second;
}

int Pieces::newRoot(int parent, int fn, std::optional<Ref> self, int callerFn, unsigned rec,
                    Mode mode) {
  for (int p = parent; p >= 0; p = this->analysis.roots[p].parent) {
    Root &up = this->analysis.roots[p];
    if (up.fn == fn && up.self == self && up.callerFn == callerFn && up.rec == int(rec)) {
      up.multi = up.recursive = true;
      return p;
    }
  }
  auto key = std::make_tuple(parent, fn, self, callerFn, rec);
  if (auto found = this->analysis.rootIndex.find(key); found != this->analysis.rootIndex.end()) {
    return found->second;
  }
  constexpr size_t kMaxRoots = 4096;
  if (this->analysis.roots.size() >= kMaxRoots) {
    for (size_t i = 0; i < this->analysis.roots.size(); ++i) {
      Root &r = this->analysis.roots[i];
      if (r.fn == fn && r.callerFn == callerFn && r.rec == int(rec)) {
        r.multi = true;
        r.parent = -1;
        return int(i);
      }
    }
  }
  this->analysis.rootIndex[key] = int(this->analysis.roots.size());
  Root &root = this->analysis.roots.emplace_back();
  root.fn = fn;
  root.self = self;
  root.parent = parent;
  root.callerFn = callerFn;
  root.rec = int(rec);
  root.mode = mode;

  return int(this->analysis.roots.size() - 1);
}

void Pieces::explore() {
  Root &main = this->analysis.roots.emplace_back();
  main.fn = this->analysis.module->id;
  std::deque<unsigned> queue{activate(this->analysis.module->id, 0, std::nullopt, true, false)};
  llvm::DenseSet<unsigned> done;
  while (!queue.empty()) {
    unsigned ai = queue.front();
    queue.pop_front();
    if (!done.insert(ai).second) {
      continue;
    }
    Activation act = this->analysis.acts[ai];
    const FnInfo &f = *this->analysis.byFunction[act.fn];
    for (unsigned r = 0; r < f.calls.size(); ++r) {
      const CallRec &rec = f.calls[r];
      for (int edgeIndex : rec.edges) {
        const CallEdge &c = this->analysis.callGraph.edge(edgeIndex);
        std::optional<Ref> self = this->analysis.callResolver.bind(c.self, act);
        unsigned next;
        switch (c.mode) {
        case Mode::Sync:
        case Mode::Await:
        case Mode::AsyncIter:
          next = activate(c.to, act.root, self, false, false);
          this->analysis.inlineEdges.push_back({ai, r, next});
          break;
        case Mode::Spawn:
          next = activate(c.to, newRoot(act.root, c.to, self, act.fn, r, Mode::Spawn), self, false,
                          true);
          break;
        case Mode::Callback:
          next = activate(c.to, newRoot(act.root, c.to, self, act.fn, r, Mode::Callback), self,
                          true, false);
          break;
        }
        this->analysis.edges.push_back({ai, r, next});
        if (!done.contains(next)) {
          queue.push_back(next);
        }
      }
    }
  }
}

void Pieces::findBindings() {
  auto &bindings = this->analysis.bindings;
  bindings.assign(this->analysis.acts.size(), {});
  std::vector<bool> reached(this->analysis.acts.size(), false);
  for (const auto &[from, r, to] : this->analysis.edges) {
    reached[to] = true;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[from, r, to] : this->analysis.edges) {
      const FunctionDecl *decl = this->analysis.byFunction[this->analysis.acts[to].fn]->decl;
      const FunctionDecl *caller = this->analysis.byFunction[this->analysis.acts[from].fn]->decl;
      const CallRec &rec = this->analysis.byFunction[this->analysis.acts[from].fn]->calls[r];
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
  this->analysis.reached = std::move(reached);
}

bool Pieces::keeps(const PieceInst &piece, const Access &a) const {
  const FunctionDecl *decl = this->analysis.byFunction[piece.fn]->decl;
  if (!a.bound || llvm::none_of(decl->params, [&](const Param &p) { return p.sym == a.bound; })) {
    return true;
  }
  for (unsigned ai : this->analysis.actsOf(piece.fn, piece.root)) {
    const Activation &act = this->analysis.acts[ai];
    if (act.self != piece.self) {
      continue;
    }
    if (!this->analysis.reached[ai]) {
      return true;
    }
    auto found = this->analysis.bindings[ai].find(a.bound);
    if (found != this->analysis.bindings[ai].end() &&
        (found->second.count(nullptr) || found->second.count(a.through))) {
      return true;
    }
  }

  return false;
}

void Pieces::findPieces() {
  this->analysis.actPieces.assign(this->analysis.acts.size(), {});
  for (unsigned ai = 0; ai < this->analysis.acts.size(); ++ai) {
    const Activation &act = this->analysis.acts[ai];
    const FnInfo &f = *this->analysis.byFunction[act.fn];
    for (unsigned s = 0; s < f.segs.size(); ++s) {
      if (!scheduled(act, s)) {
        continue;
      }
      auto key = std::make_tuple(act.fn, s, act.root, act.self);
      auto [it, inserted] = this->analysis.pieceIndex.try_emplace(
          key, unsigned(this->analysis.pieceInstances.size()));
      if (inserted) {
        this->analysis.pieceInstances.push_back({act.fn, s, act.root, act.self});
        this->analysis.segPieces[{act.fn, s}].push_back(it->second);
        ++this->analysis.sameTask[std::make_tuple(act.fn, s, act.root)];
      }
      this->analysis.actPieces[ai].push_back(it->second);
    }
  }
}

bool Pieces::ownPiece(unsigned act, unsigned piece) const {
  return piece < this->analysis.pieceInstances.size() &&
         llvm::is_contained(this->analysis.actPieces[act], piece);
}

void Pieces::addEntry(unsigned act, unsigned piece, const Entry &add, bool &changed) {
  auto [it, inserted] = this->analysis.entries.try_emplace({act, piece}, add);
  if (inserted) {
    changed = true;
    return;
  }
  Entry &cur = it->second;
  if (cur.open) {
    return;
  }
  if (add.open) {
    cur.open = changed = true;
    return;
  }
  const ExplainNames &names = this->analysis.explainNames;
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
    for (const auto &[caller, r, callee] : this->analysis.inlineEdges) {
      const Activation &from = this->analysis.acts[caller];
      const CallRec &rec = this->analysis.byFunction[from.fn]->calls[r];
      std::vector<unsigned> at = piecesAt(from, rec.segs);
      Activation &to = this->analysis.acts[callee];
      for (unsigned p : at) {
        changed |= to.hosts.insert(p).second;
        Entry add;
        if (ownPiece(caller, p)) {
          add.first = add.last = rec.loc;
        } else if (auto e = this->analysis.entries.find({caller, p});
                   e != this->analysis.entries.end()) {
          add = e->second;
        } else {
          add.open = true;
        }
        addEntry(callee, p, add, changed);
      }
    }
  }
}

int Pieces::stepOf(int fn, llvm::SMLoc loc) {
  if (!loc.isValid()) {
    return -1;
  }
  auto [it, inserted] = this->analysis.steps.try_emplace(fn);
  std::vector<Pos> &steps = it->second;
  const ExplainNames &names = this->analysis.explainNames;
  if (inserted) {
    const FunctionDecl *decl = this->analysis.byFunction[fn]->decl;
    if (decl == this->analysis.module) {
      for (const Stmt *s : this->analysis.program.items) {
        if (!isa<FunctionDecl>(s) && !this->analysis.libraryEffects.inLibrary(s->loc())) {
          steps.push_back(names.posOf(s->loc()));
        }
      }
    } else if (decl->body) {
      for (const Stmt *s : decl->body->body) {
        steps.push_back(names.posOf(s->loc()));
      }
    }
    llvm::sort(steps);
  }
  Pos at = names.posOf(loc);
  auto next = std::upper_bound(steps.begin(), steps.end(), at);
  if (next == steps.begin() || std::prev(next)->buffer != at.buffer) {
    return -1;
  }

  return int(next - steps.begin()) - 1;
}

bool Pieces::later(int fn, llvm::SMLoc at, llvm::SMLoc origin) {
  int from = stepOf(fn, at);
  int to = stepOf(fn, origin);

  return from < 0 || to < 0 || to >= from;
}

void Pieces::noteFlow(int root, unsigned from, unsigned to, llvm::SMLoc origin) {
  auto &origins = this->analysis.flowFrom[root];
  auto [it, inserted] = origins.try_emplace({from, to}, origin);
  if (inserted || !it->second.isValid()) {
    return;
  }
  if (!origin.isValid()) {
    it->second = origin;
    return;
  }
  const ExplainNames &names = this->analysis.explainNames;
  if (names.posOf(it->second) < names.posOf(origin)) {
    it->second = origin;
  }
}

std::vector<unsigned> Pieces::piecesAt(const Activation &act, llvm::ArrayRef<unsigned> segs) const {
  std::vector<unsigned> out;
  const FnInfo &f = *this->analysis.byFunction[act.fn];
  for (unsigned s : segs) {
    if (scheduled(act, s)) {
      auto it = this->analysis.pieceIndex.find(std::make_tuple(act.fn, s, act.root, act.self));
      if (it != this->analysis.pieceIndex.end()) {
        out.push_back(it->second);
      }
    } else if (f.isInline(s)) {
      if (act.rootStart) {
        out.push_back(this->analysis.startNode(act.root));
      } else {
        out.insert(out.end(), act.hosts.begin(), act.hosts.end());
      }
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());

  return out;
}

bool Pieces::once(int fi, unsigned rec, int rootFn, unsigned depth) {
  if (this->analysis.byFunction[fi]->calls[rec].repeated) {
    return false;
  }

  return fnOnce(fi, rootFn, depth);
}

bool Pieces::fnOnce(int fi, int rootFn, unsigned depth) {
  if (fi == rootFn) {
    return true;
  }
  if (depth > 32 || this->analysis.valueFns.contains(fi)) {
    return false;
  }
  auto key = std::make_pair(fi, rootFn);
  auto found = this->analysis.fnOnce.find(key);
  if (found != this->analysis.fnOnce.end()) {
    return found->second;
  }
  this->analysis.fnOnce[key] = false;
  const std::vector<int> &incoming = this->analysis.callGraph.incoming(fi);
  bool result = false;
  if (incoming.size() == 1) {
    const CallEdge &call = this->analysis.callGraph.edge(incoming[0]);
    bool direct = !call.byType && (call.mode == Mode::Sync || call.mode == Mode::Await ||
                                   call.mode == Mode::AsyncIter);
    result = direct && once(call.from, unsigned(call.call), rootFn, depth + 1);
  }

  return this->analysis.fnOnce[key] = result;
}

bool Pieces::rejectionEnds(const AwaitExpr *a, unsigned depth) {
  auto found = this->analysis.awaits.find(a);
  if (found == this->analysis.awaits.end()) {
    return false;
  }

  return fnRejectionEnds(found->second.fn, depth);
}

bool Pieces::fnRejectionEnds(int fi, unsigned depth) {
  if (fi == this->analysis.module->id) {
    return true;
  }
  if (depth > 32 || this->analysis.valueFns.contains(fi) || !this->analysis.byFunction[fi]->async) {
    return false;
  }
  auto found = this->analysis.rejectionEnds.find(fi);
  if (found != this->analysis.rejectionEnds.end()) {
    return found->second;
  }
  this->analysis.rejectionEnds[fi] = false;
  bool result = true;
  for (int edgeIndex : this->analysis.callGraph.incoming(fi)) {
    const CallEdge &c = this->analysis.callGraph.edge(edgeIndex);
    const CallRec &rec = this->analysis.byFunction[c.from]->calls[c.call];
    bool ends = false;
    switch (c.mode) {
    case Mode::Await:
      ends = rec.await ? rejectionEnds(rec.await, depth + 1)
                       : rec.returned && fnRejectionEnds(c.from, depth + 1);
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

  return this->analysis.rejectionEnds[fi] = result;
}

void Pieces::findJoins() {
  for (size_t i = 1; i < this->analysis.roots.size(); ++i) {
    Root &root = this->analysis.roots[i];
    if (root.rec < 0) {
      continue;
    }
    const FnInfo &caller = *this->analysis.byFunction[root.callerFn];
    const CallRec &rec = caller.calls[root.rec];
    std::optional<int> parentFn;
    if (root.parent >= 0) {
      parentFn = this->analysis.roots[root.parent].fn;
    }
    root.multi |= rec.interval || !parentFn || !once(root.callerFn, root.rec, *parentFn, 0) ||
                  (size_t(root.parent) < i && this->analysis.roots[root.parent].multi);
    const FunctionDecl *callee = this->analysis.byFunction[root.fn]->decl;
    if (rec.kind == CallKind::Inline && rec.call && rec.call->builtin() == Builtin::ArrMap &&
        callee && callee->params.size() > 1 && callee->params[1].sym &&
        this->analysis.ownIndex.contains(callee->params[1].sym) && !rec.inLoop && parentFn &&
        fnOnce(root.callerFn, *parentFn, 0)) {
      bool single = true;
      for (int p = root.parent; p >= 0; p = this->analysis.roots[p].parent) {
        single &= !this->analysis.roots[p].multi;
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
    for (unsigned ai : this->analysis.actsOf(root.callerFn, root.parent)) {
      for (unsigned p : piecesAt(this->analysis.acts[ai], rec.segs)) {
        spawnAt.insert(p);
        llvm::SMLoc at;
        if (ownPiece(ai, p)) {
          at = rec.loc;
        } else if (auto e = this->analysis.entries.find({ai, p});
                   e != this->analysis.entries.end() && !e->second.open) {
          at = e->second.first;
        }
        auto [it, inserted] = spawnFrom.try_emplace(p, at);
        if (inserted || !it->second.isValid()) {
          continue;
        }
        if (!at.isValid() ||
            this->analysis.explainNames.posOf(at) < this->analysis.explainNames.posOf(it->second)) {
          it->second = at;
        }
      }
    }
    root.spawnAt.assign(spawnAt.begin(), spawnAt.end());
    root.spawnFrom.assign(spawnFrom.begin(), spawnFrom.end());
    if (root.mode != Mode::Spawn || !rec.join) {
      continue;
    }
    auto join = this->analysis.joins.find(rec.join);
    if (join == this->analysis.joins.end() || !rejectionEnds(rec.join, 0)) {
      continue;
    }
    bool found = false;
    for (unsigned ai : this->analysis.actsOf(join->second.fn, root.parent)) {
      found = true;
      for (unsigned p : piecesAt(this->analysis.acts[ai], join->second.segs)) {
        interval.insert(p);
      }
    }
    if (!found) {
      continue;
    }
    root.join = rec.join;
    root.interval.assign(interval.begin(), interval.end());
  }
  for (size_t i = 1; i < this->analysis.roots.size(); ++i) {
    Root &root = this->analysis.roots[i];
    if (root.parent < 0 || size_t(root.parent) >= i) {
      continue;
    }
    const Root &parent = this->analysis.roots[root.parent];
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
  this->analysis.flow.assign(this->analysis.roots.size(), {});
  this->analysis.flowFrom.assign(this->analysis.roots.size(), {});
  for (unsigned ai = 0; ai < this->analysis.acts.size(); ++ai) {
    const Activation &act = this->analysis.acts[ai];
    const FnInfo &f = *this->analysis.byFunction[act.fn];
    for (const auto &[a, b] : f.succ) {
      for (unsigned x : piecesAt(act, {a})) {
        llvm::SMLoc origin;
        if (scheduled(act, a)) {
          origin = f.segs[b].loc;
        } else if (auto e = this->analysis.entries.find({ai, x});
                   e != this->analysis.entries.end() && !e->second.open) {
          origin = e->second.last;
        }
        for (unsigned y : piecesAt(act, {b})) {
          this->analysis.flow[act.root][x].push_back(y);
          noteFlow(act.root, x, y, origin);
        }
      }
    }
  }
  for (const auto &[caller, r, callee] : this->analysis.inlineEdges) {
    const Activation &x = this->analysis.acts[caller];
    if (this->analysis.acts[callee].root != x.root || this->analysis.actPieces[callee].empty()) {
      continue;
    }
    const FnInfo &f = *this->analysis.byFunction[x.fn];
    const CallRec &rec = f.calls[r];
    std::vector<unsigned> after;
    for (const auto &[a, b] : f.succ) {
      if (llvm::is_contained(rec.segs, a)) {
        for (unsigned n : piecesAt(x, {b})) {
          after.push_back(n);
        }
      }
    }
    for (unsigned p : this->analysis.actPieces[callee]) {
      std::vector<unsigned> &next = this->analysis.flow[x.root][p];
      next.insert(next.end(), after.begin(), after.end());
      for (unsigned n : after) {
        noteFlow(x.root, p, n, llvm::SMLoc());
      }
    }
  }
  for (auto &graph : this->analysis.flow) {
    for (auto &[node, next] : graph) {
      std::sort(next.begin(), next.end());
      next.erase(std::unique(next.begin(), next.end()), next.end());
    }
  }
}

std::vector<unsigned> Pieces::reach(int root, const std::vector<unsigned> &from) const {
  std::set<unsigned> seen(from.begin(), from.end());
  std::vector<unsigned> pending(from.begin(), from.end());
  const auto &graph = this->analysis.flow[root];
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
  const auto &graph = this->analysis.flow[root];
  const auto &origins = this->analysis.flowFrom[root];
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
                   p >= this->analysis.pieceInstances.size() ||
                   later(this->analysis.pieceInstances[p].fn, at, origin->second);
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
