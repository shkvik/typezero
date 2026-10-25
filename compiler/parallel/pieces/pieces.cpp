#include "compiler/parallel/pieces/pieces.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/expr_walk/expr_walk.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/library_effects/library_effects.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/parallel/explainer/explainer.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
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
#include "compiler/parallel/parallel_model/parallel_model.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz::parallel {

Pieces::Pieces(Analysis &analysis) : analysis_(analysis) {}

bool Pieces::scheduled(const Activation &act, unsigned seg) const {
  const FnInfo &f = analysis_.fns_[act.fn];
  if (seg == 0) {
    return act.callbackStart;
  }

  return f.segs[seg].kind == SegKind::Await || f.segs[seg].kind == SegKind::ForAwait;
}

unsigned Pieces::activate(unsigned fn, int root, std::optional<Ref> self, bool callbackStart,
                            bool rootStart) {
  auto key = std::make_tuple(fn, root, self, callbackStart);
  auto [it, inserted] = analysis_.actIndex_.try_emplace(key, unsigned(analysis_.acts_.size()));
  if (inserted) {
    Activation &act = analysis_.acts_.emplace_back();
    act.fn = fn;
    act.root = root;
    act.self = self;
    act.callbackStart = callbackStart;
    analysis_.actsByTask_[{fn, root}].push_back(it->second);
  }
  analysis_.acts_[it->second].rootStart |= rootStart;

  return it->second;
}

int Pieces::newRoot(int parent, unsigned fn, std::optional<Ref> self, unsigned callerFn,
                      unsigned rec, Mode mode) {
  // A task that starts itself again down its chain is that task again:
  // several of it may run at once.
  for (int p = parent; p >= 0; p = analysis_.roots_[p].parent) {
    Root &up = analysis_.roots_[p];
    if (up.fn == fn && up.self == self && up.callerFn == callerFn && up.rec == int(rec)) {
      up.multi = up.recursive = true;
      return p;
    }
  }
  auto key = std::make_tuple(parent, fn, self, callerFn, rec);
  if (auto found = analysis_.rootIndex_.find(key); found != analysis_.rootIndex_.end()) {
    return found->second;
  }
  // A program that makes too many kinds of tasks gets them told apart less.
  constexpr size_t kMaxRoots = 4096;
  if (analysis_.roots_.size() >= kMaxRoots) {
    for (size_t i = 0; i < analysis_.roots_.size(); ++i) {
      Root &r = analysis_.roots_[i];
      if (r.fn == fn && r.callerFn == callerFn && r.rec == int(rec)) {
        r.multi = true;
        r.parent = -1;
        return int(i);
      }
    }
  }
  analysis_.rootIndex_[key] = int(analysis_.roots_.size());
  Root &root = analysis_.roots_.emplace_back();
  root.fn = fn;
  root.self = self;
  root.parent = parent;
  root.callerFn = callerFn;
  root.rec = int(rec);
  root.mode = mode;

  return int(analysis_.roots_.size() - 1);
}

void Pieces::explore() {
  Root &main = analysis_.roots_.emplace_back();
  main.fn = 0;
  std::deque<unsigned> queue{activate(0, 0, std::nullopt, true, false)};
  llvm::DenseSet<unsigned> done;
  while (!queue.empty()) {
    unsigned ai = queue.front();
    queue.pop_front();
    if (!done.insert(ai).second) {
      continue;
    }
    Activation act = analysis_.acts_[ai];
    const FnInfo &f = analysis_.fns_[act.fn];
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
          analysis_.inlineEdges_.push_back({ai, r, next});
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
        if (!done.contains(next)) {
          queue.push_back(next);
        }
      }
    }
  }
}

void Pieces::findPieces() {
  analysis_.actPieces_.assign(analysis_.acts_.size(), {});
  for (unsigned ai = 0; ai < analysis_.acts_.size(); ++ai) {
    const Activation &act = analysis_.acts_[ai];
    const FnInfo &f = analysis_.fns_[act.fn];
    for (unsigned s = 0; s < f.segs.size(); ++s) {
      if (!scheduled(act, s)) {
        continue;
      }
      auto key = std::make_tuple(act.fn, s, act.root, act.self);
      auto [it, inserted] = analysis_.pieceIndex_.try_emplace(key, unsigned(analysis_.pieces_.size()));
      if (inserted) {
        analysis_.pieces_.push_back({act.fn, s, act.root, act.self});
        analysis_.segPieces_[{act.fn, s}].push_back(it->second);
        ++analysis_.sameTask_[std::make_tuple(act.fn, s, act.root)];
      }
      analysis_.actPieces_[ai].push_back(it->second);
    }
  }
}

// The pieces of its task that a function's inline segments run inside.
void Pieces::findHosts() {
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[caller, r, callee] : analysis_.inlineEdges_) {
      const Activation &from = analysis_.acts_[caller];
      std::vector<unsigned> at = piecesAt(from, analysis_.fns_[from.fn].calls[r].segs);
      Activation &to = analysis_.acts_[callee];
      for (unsigned p : at) {
        changed |= to.hosts.insert(p).second;
      }
    }
  }
}

std::vector<unsigned> Pieces::piecesAt(const Activation &act,
                                         llvm::ArrayRef<unsigned> segs) const {
  std::vector<unsigned> out;
  const FnInfo &f = analysis_.fns_[act.fn];
  for (unsigned s : segs) {
    if (scheduled(act, s)) {
      auto it = analysis_.pieceIndex_.find(std::make_tuple(act.fn, s, act.root, act.self));
      if (it != analysis_.pieceIndex_.end()) {
        out.push_back(it->second);
      }
    } else if (f.isInline(s)) {
      // Its start runs inside the piece that calls it; the start of a task
      // runs inside the piece of another task.
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

// Whether the call runs at most once in one run of the task that runs
// `rootFn`.
bool Pieces::once(unsigned fi, unsigned rec, unsigned rootFn, unsigned depth) {
  if (analysis_.fns_[fi].calls[rec].repeated) {
    return false;
  }

  return fnOnce(fi, rootFn, depth);
}

bool Pieces::fnOnce(unsigned fi, unsigned rootFn, unsigned depth) {
  if (fi == rootFn) {
    return true;
  }
  if (depth > 32 || analysis_.valueFns_.contains(fi)) {
    return false;
  }
  auto key = std::make_pair(fi, rootFn);
  auto found = analysis_.fnOnce_.find(key);
  if (found != analysis_.fnOnce_.end()) {
    return found->second;
  }
  analysis_.fnOnce_[key] = false; // a recursive function runs more than once
  const FnInfo &f = analysis_.fns_[fi];
  bool result = false;
  if (f.named.size() == 1) {
    auto [cf, cr] = f.named[0];
    const CallRec &rec = analysis_.fns_[cf].calls[cr];
    bool direct = llvm::any_of(rec.callees, [&](const Callee &c) {
      return c.fn == fi && !c.byType &&
             (c.mode == Mode::Sync || c.mode == Mode::Await || c.mode == Mode::AsyncIter);
    });
    result = direct && once(cf, cr, rootFn, depth + 1);
  }

  return analysis_.fnOnce_[key] = result;
}

// Whether an error that the await throws ends the program before any other
// code runs: then no code after it may run while the tasks it waited for
// still do.
bool Pieces::rejectionEnds(const AwaitExpr *a, unsigned depth) {
  auto found = analysis_.awaits_.find(a);
  if (found == analysis_.awaits_.end() || found->second.caught) {
    return false;
  }

  return fnRejectionEnds(found->second.fn, depth);
}

bool Pieces::fnRejectionEnds(unsigned fi, unsigned depth) {
  if (fi == 0) {
    return true; // the top-level code: an error there ends the program
  }
  if (depth > 32 || analysis_.valueFns_.contains(fi) || !analysis_.fns_[fi].async) {
    return false;
  }
  auto found = analysis_.rejectionEnds_.find(fi);
  if (found != analysis_.rejectionEnds_.end()) {
    return found->second;
  }
  analysis_.rejectionEnds_[fi] = false;
  bool result = true;
  for (const auto &[cf, cr] : analysis_.fns_[fi].named) {
    const CallRec &rec = analysis_.fns_[cf].calls[cr];
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
        // `void f()`: an error nobody handles ends the program.
        ends = rec.voided ||
               (rec.join && !analysis_.joins_.lookup(rec.join).settled && rejectionEnds(rec.join, depth + 1));
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

  return analysis_.rejectionEnds_[fi] = result;
}

void Pieces::findJoins() {
  for (size_t i = 1; i < analysis_.roots_.size(); ++i) {
    Root &root = analysis_.roots_[i];
    if (root.rec < 0) {
      continue;
    }
    const FnInfo &caller = analysis_.fns_[root.callerFn];
    const CallRec &rec = caller.calls[root.rec];
    int parentFn = root.parent >= 0 ? int(analysis_.roots_[root.parent].fn) : -1;
    root.multi |= rec.interval || parentFn < 0 || !once(root.callerFn, root.rec, parentFn, 0);
    // Each copy of a task that map starts for an element writes the element
    // of its own index: if the map runs once, no two copies share one.
    const FunctionDecl *callee = analysis_.fns_[root.fn].decl;
    if (rec.kind == CallKind::Inline && rec.call && rec.call->builtin == Builtin::ArrMap &&
        callee && callee->params.size() > 1 && callee->params[1].sym &&
        analysis_.ownIndex_.contains(callee->params[1].sym) && !rec.inLoop && parentFn >= 0 &&
        fnOnce(root.callerFn, unsigned(parentFn), 0)) {
      bool single = true;
      for (int p = root.parent; p >= 0; p = analysis_.roots_[p].parent) {
        single &= !analysis_.roots_[p].multi;
      }
      if (single) {
        root.ownCell = callee;
      }
    }
    if (root.parent < 0) {
      continue;
    }
    // Where it starts: the pieces of the parent that make the call.
    std::set<unsigned> spawnAt, interval;
    for (unsigned ai : analysis_.actsOf(root.callerFn, root.parent)) {
      for (unsigned p : piecesAt(analysis_.acts_[ai], rec.segs)) {
        spawnAt.insert(p);
      }
    }
    root.spawnAt.assign(spawnAt.begin(), spawnAt.end());
    if (root.mode != Mode::Spawn || !rec.join) {
      continue;
    }
    auto join = analysis_.joins_.find(rec.join);
    // An error there, of a task or of the code that starts them, must end
    // the program; otherwise code could go on while some still run.
    if (join == analysis_.joins_.end() || !rejectionEnds(rec.join, 0)) {
      continue;
    }
    // The pieces of the parent while it runs: those where the Promise.all
    // waits.
    bool found = false;
    for (unsigned ai : analysis_.actsOf(join->second.fn, root.parent)) {
      found = true;
      for (unsigned p : piecesAt(analysis_.acts_[ai], join->second.segs)) {
        interval.insert(p);
      }
    }
    if (!found) {
      continue;
    }
    root.join = rec.join;
    root.interval.assign(interval.begin(), interval.end());
  }
  // The chains, parents first: a task is made after its parent.
  for (size_t i = 1; i < analysis_.roots_.size(); ++i) {
    Root &root = analysis_.roots_[i];
    if (root.parent < 0 || size_t(root.parent) >= i) {
      continue;
    }
    const Root &parent = analysis_.roots_[root.parent];
    if (root.join) {
      root.chain.push_back({root.parent, root.interval});
      root.chain.insert(root.chain.end(), parent.chain.begin(), parent.chain.end());
      continue;
    }
    // Nothing waits for it: it may run on after its parent, and after the
    // task that waits for its parent.
    root.chain.push_back({root.parent, reach(root.parent, root.spawnAt)});
    for (const auto &[up, pieces] : parent.chain) {
      root.chain.push_back({up, reach(up, pieces)});
    }
  }
}

// The pieces of each task that may follow each other: within a function
// from await to await, and around an awaited call, whose pieces come after
// the piece that calls it and before the pieces that follow.
void Pieces::buildFlow() {
  analysis_.flow_.assign(analysis_.roots_.size(), {});
  for (const Activation &act : analysis_.acts_) {
    const FnInfo &f = analysis_.fns_[act.fn];
    for (const auto &[a, b] : f.succ) {
      for (unsigned x : piecesAt(act, {a})) {
        for (unsigned y : piecesAt(act, {b})) {
          analysis_.flow_[act.root][x].push_back(y);
        }
      }
    }
  }
  for (const auto &[caller, r, callee] : analysis_.inlineEdges_) {
    const Activation &x = analysis_.acts_[caller];
    if (analysis_.acts_[callee].root != x.root || analysis_.actPieces_[callee].empty()) {
      continue;
    }
    const FnInfo &f = analysis_.fns_[x.fn];
    const CallRec &rec = f.calls[r];
    std::vector<unsigned> after;
    for (const auto &[a, b] : f.succ) {
      if (llvm::is_contained(rec.segs, a)) {
        for (unsigned n : piecesAt(x, {b})) {
          after.push_back(n);
        }
      }
    }
    for (unsigned p : analysis_.actPieces_[callee]) {
      std::vector<unsigned> &next = analysis_.flow_[x.root][p];
      next.insert(next.end(), after.begin(), after.end());
    }
  }
  for (auto &graph : analysis_.flow_) {
    for (auto &[node, next] : graph) {
      std::sort(next.begin(), next.end());
      next.erase(std::unique(next.begin(), next.end()), next.end());
    }
  }
}

// The pieces of the task that may run from those of `from` on, them
// included.
std::vector<unsigned> Pieces::reach(int root, const std::vector<unsigned> &from) const {
  std::set<unsigned> seen(from.begin(), from.end());
  std::vector<unsigned> pending(from.begin(), from.end());
  const auto &graph = analysis_.flow_[root];
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

} // namespace tz::parallel
