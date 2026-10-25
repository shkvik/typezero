#include "compiler/overflow/loop_ranges/loop_ranges.h"
#include "compiler/overflow/program_scan/program_scan.h"
#include "compiler/overflow/slots/slots.h"
#include "compiler/overflow/call_ranges/call_ranges.h"
#include "compiler/overflow/executor/executor.h"
#include "compiler/overflow/narrowing/narrowing.h"
#include "compiler/overflow/evaluator/evaluator.h"
#include "compiler/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/check/facts/facts.h"
#include "compiler/check/sema/sema.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "compiler/overflow/value_range/value_range.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

// A loop that runs at most this many passes is run pass by pass.
constexpr double kUnrollPasses = 24;

// Whether `stmt` yields, which makes whoever iterates the generator decide
// how many passes its loop makes.
bool yields(const Stmt *stmt) {
  bool found = false;
  visitExprs(stmt, [&](const Expr *e) { found |= isa<YieldExpr>(e); }, /*intoFunctions=*/false);

  return found;
}

// Whether a loop with the condition `cond` (null: none) is open-ended: no
// condition, a constant one, or one that reads the input.
bool openEnded(const Expr *cond, const Stmt *body) {
  if (yields(body)) {
    return false;
  }
  if (!cond) {
    return true;
  }
  if (auto *lit = dyn_cast<BoolLitExpr>(stripCasts(cond))) {
    return lit->value;
  }
  bool input = false;
  visitExprs(cond, [&](const Expr *e) {
    if (auto *call = dyn_cast<CallExpr>(e)) {
      input |= call->builtin == Builtin::ReadLine || isFsBuiltin(call->builtin) ||
               isChildBuiltin(call->builtin);
    }
  });

  return input;
}

// Whether `body` makes the array in `var` longer.
bool grows(const Stmt *body, const VarSymbol *var) {
  auto isVar = [&](const Expr *e) {
    auto *name = dyn_cast_or_null<NameExpr>(stripCasts(e));

    return name && name->var == var;
  };
  bool found = false;
  visitExprs(body, [&](const Expr *e) {
    if (auto *call = dyn_cast<CallExpr>(e)) {
      auto *member = dyn_cast<MemberExpr>(call->callee);
      if (member && isVar(member->object) &&
          (call->builtin == Builtin::ArrPush || call->builtin == Builtin::ArrUnshift ||
           call->builtin == Builtin::ArrSplice)) {
        found = true;
      }
    } else if (auto *assign = dyn_cast<AssignExpr>(e)) {
      if (auto *index = dyn_cast<IndexExpr>(assign->target);
          index && isVar(index->object) && index->mayAppend) {
        found = true;
      }
      if (auto *member = dyn_cast<MemberExpr>(assign->target);
          member && member->name == "length" && isVar(member->object)) {
        found = true;
      }
    }
  });

  return found;
}

// How far a pass moved a value from what it was at the head: [offLo, offHi]
// if it only added to it.
std::optional<std::pair<double, double>> stepOf(const Num &end, uint64_t base) {
  if (end.empty() || end.base != base || !base) {
    return std::nullopt;
  }

  return std::make_pair(end.offLo, end.offHi);
}

} // namespace

LoopRanges::LoopRanges(OverflowCheck &check) : check_(check) {}

void LoopRanges::execLoop(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels, State &st) {
  OverflowCheck::LoopSpec spec;
  spec.loc = stmt->loc();
  spec.labels.assign(labels.begin(), labels.end());
  if (auto *s = dyn_cast<WhileStmt>(stmt)) {
    spec.cond = s->cond;
    spec.openEnded = openEnded(s->cond, s->body);
    spec.simple = !containsLoop(s->body);
    spec.body = [this, s](State &in) { check_.executor().exec(s->body, in); };
    runLoop(spec, st);
    return;
  }
  if (auto *s = dyn_cast<DoWhileStmt>(stmt)) {
    spec.condAfter = s->cond;
    spec.openEnded = openEnded(s->cond, s->body);
    spec.simple = !containsLoop(s->body);
    spec.body = [this, s](State &in) { check_.executor().exec(s->body, in); };
    runLoop(spec, st);
    return;
  }
  if (auto *s = dyn_cast<ForStmt>(stmt)) {
    check_.executor().exec(s->init, st);
    spec.cond = s->cond;
    spec.step = s->step;
    spec.openEnded = openEnded(s->cond, s->body);
    spec.simple = !containsLoop(s->body);
    spec.body = [this, s](State &in) { check_.executor().exec(s->body, in); };
    runLoop(spec, st);
    return;
  }
  auto *s = cast<ForOfStmt>(stmt);
  Val iterable = check_.evaluator().eval(s->iterable, st);
  const Type *type = s->iterable->type ? s->iterable->type->nonOptional() : nullptr;
  const Type *bound = s->binding.sym ? s->binding.sym->type : nullptr;
  auto *name = dyn_cast<NameExpr>(stripCasts(s->iterable));
  const VarSymbol *iterated = name && st.vars.count(name->var) ? name->var : nullptr;
  Val element = check_.overflowWarnings().trusted(bound);
  if (type && type->isArray()) {
    element = check_.slots().elementsOf(s->iterable, iterable, type);
    // An array that the loop makes longer goes on with the new elements.
    if (!iterated || !grows(s->body, iterated)) {
      spec.passes = add(iterable.len, exactly(1));
    }
  } else if (type && type->isString()) {
    element = lengthVal(range(1, 2));
    spec.passes = add(iterable.len, exactly(1));
  } else if (type && type->isDict()) {
    spec.passes = someLength();
  }
  spec.exitAtHead = true;
  spec.simple = !containsLoop(s->body);
  spec.body = [this, s, type, element, iterated](State &in) {
    // An array the loop changes: its elements as they are on this pass.
    Val e = element;
    if (iterated && type && type->isArray()) {
      e = check_.slots().elementsOf(s->iterable, check_.slots().readVar(iterated, in), type);
    }
    check_.slots().writeVar(s->binding.sym, e, in);
    check_.executor().exec(s->body, in);
  };
  runLoop(spec, st);
}

State LoopRanges::onePass(const OverflowCheck::LoopSpec &spec, const State &head, State &exits) {
  ++check_.work_;
  State s = head;
  if (spec.exitAtHead) {
    exits = join(exits, s);
  }
  if (spec.cond) {
    State t, f;
    check_.narrowing().refine(spec.cond, s, t, f);
    exits = join(exits, f);
    s = t;
  }
  OverflowCheck::Target target;
  target.loop = true;
  target.labels = spec.labels;
  target.quiet = check_.quiet_;
  check_.targets_.push_back(std::move(target));
  if (!s.dead) {
    spec.body(s);
  }
  State conts = check_.targets_.back().conts, breaks = check_.targets_.back().breaks;
  check_.targets_.pop_back();
  s = join(s, conts);
  exits = join(exits, breaks);
  if (spec.step && !s.dead) {
    check_.evaluator().eval(spec.step, s);
  }
  if (spec.condAfter && !s.dead) {
    State t, f;
    check_.narrowing().refine(spec.condAfter, s, t, f);
    exits = join(exits, f);
    s = t;
  }

  return s;
}

State LoopRanges::rebased(const State &state,
                             llvm::DenseMap<const VarSymbol *, uint64_t> &bases) {
  State r = state;
  for (auto &entry : r.vars) {
    uint64_t &base = bases[entry.first];
    if (!base) {
      base = ++check_.bases_;
      ++check_.bases_;
    }
    entry.second.num.base = base;
    entry.second.num.offLo = entry.second.num.offHi = 0;
    entry.second.len.base = base + 1;
    entry.second.len.offLo = entry.second.len.offHi = 0;
  }

  return r;
}

Num LoopRanges::passCount(const OverflowCheck::LoopSpec &spec, const State &entry, const State &head,
                             const State &first, const State &firstEnd) {
  if (firstEnd.dead) {
    return spec.passes;
  }
  Num best = spec.passes;
  if (!best.empty()) {
    best = make(Bound{0}, Bound{std::max(best.hi, 0.0), best.hiOrigin, best.hiWhy});
  }
  auto consider = [&](double n, const Bound &limit) {
    if (!(n >= 0) || n == kInf) {
      return;
    }
    if (best.empty() || n < best.hi) {
      best = make(Bound{0}, Bound{std::floor(n), limit.origin, limit.why});
    }
  };
  for (const auto &entryVar : entry.vars) {
    const VarSymbol *var = entryVar.first;
    auto h = head.vars.find(var), f = first.vars.find(var), e = firstEnd.vars.find(var);
    if (h == head.vars.end() || f == first.vars.end() || e == firstEnd.vars.end()) {
      continue;
    }
    const Num &start = entryVar.second.num;
    // A counter that moves by the same step on every pass, which the loop's
    // checks keep within bounds.
    if (std::optional<std::pair<double, double>> step = stepOf(e->second.num, f->second.num.base);
        step && step->first == step->second && step->first != 0 && isIntegerType(var->type) &&
        !start.empty()) {
      double s = step->first;
      Limits l = limitsOf(var->type);
      const Num &bounded = h->second.num;
      if (s > 0 && bounded.hi < l.hi - s) {
        consider((bounded.hi - start.lo) / s + 1, bounded.high());
      }
      if (s < 0 && bounded.lo > l.lo - s) {
        consider((start.hi - bounded.lo) / -s + 1, bounded.low());
      }
    }
    // A length that shrinks on every pass: it cannot go below 0.
    if (std::optional<std::pair<double, double>> step = stepOf(e->second.len, f->second.len.base);
        step && step->second < 0 && !entryVar.second.len.empty()) {
      consider(entryVar.second.len.hi / -step->second + 1, entryVar.second.len.high());
    }
  }

  return best;
}

void LoopRanges::runLoop(const OverflowCheck::LoopSpec &spec, State &st) {
  if (st.dead) {
    return;
  }
  const State entry = st;
  // The passes before the last only find the head of the loop: what holds at
  // the start of every pass. Nothing is recorded in them, and values are not
  // wrapped around in them, so that a sum keeps its relation to the head:
  // each pass starts with every value relative to itself, and what a pass
  // adds to a value is its step.
  ++check_.quiet_;
  llvm::DenseMap<const VarSymbol *, uint64_t> bases;

  // 1. Until the head holds what the passes give, every end that moves goes
  //    to the end of its type's range.
  State head = entry;
  State lastHead, lastEnd = deadState();
  bool comesBack = false;
  for (int k = 0; k < (check_.overBudget() ? 2 : 6); ++k) {
    State from = rebased(head, bases);
    State exits = deadState();
    State end = onePass(spec, from, exits);
    if (end.dead) {
      break;
    }
    comesBack = true;
    bool moved = false;
    State next = join(entry, end);
    for (auto &headVar : head.vars) {
      const VarSymbol *var = headVar.first;
      auto found = next.vars.find(var);
      if (found == next.vars.end()) {
        continue;
      }
      Val &h = headVar.second;
      const Val &n = found->second;
      if (isBottom(h)) {
        // Set in the loop for the first time: what the passes before set.
        if (!isBottom(n)) {
          h = n;
          h.num.base = h.len.base = 0;
          moved = true;
        }
        continue;
      }
      auto widen = [&](Num &to, const Num &got, Limits l) {
        if (to.empty() || got.empty()) {
          return;
        }
        if (got.lo < to.lo && to.lo > l.lo) {
          to.setLow({l.lo, got.loOrigin, got.loWhy});
          moved = true;
        }
        if (got.hi > to.hi && to.hi < l.hi) {
          to.setHigh({l.hi, got.hiOrigin, got.hiWhy});
          moved = true;
        }
        to.base = 0;
      };
      widen(h.num, n.num, limitsOf(var->type));
      widen(h.len, n.len, {0, kLengthMax});
      const Type *element =
          var->type->nonOptional()->isArray() ? var->type->nonOptional()->inner() : nullptr;
      if (h.elem && n.elem && !sameVal(*h.elem, join(*h.elem, *n.elem))) {
        h.elem = share(widenVal(*h.elem, join(*h.elem, *n.elem), element));
        moved = true;
      } else if (h.elem && !n.elem) {
        h.elem = nullptr;
        moved = true;
      }
    }
    if (!moved) {
      break;
    }
  }
  if (!comesBack) {
    // The body never comes back for another pass.
    --check_.quiet_;
    State exits = deadState();
    onePass(spec, entry, exits);
    st = exits;
    return;
  }

  // 2. Passes from that head bring back the bounds that the checks and the
  //    body keep: `i < n` bounds i.
  for (int k = 0; k < (check_.overBudget() ? 1 : 2); ++k) {
    State from = rebased(head, bases);
    State exits = deadState();
    State end = onePass(spec, from, exits);
    if (end.dead) {
      break;
    }
    lastEnd = end;
    lastHead = from;
    State next = join(entry, end);
    for (auto &headVar : head.vars) {
      auto found = next.vars.find(headVar.first);
      if (found == next.vars.end()) {
        continue;
      }
      Val &h = headVar.second;
      const Val &n = found->second;
      if (!h.num.empty() && !n.num.empty()) {
        h.num = meet(h.num, n.num);
      }
      if (!h.len.empty() && !n.len.empty()) {
        h.len = meet(h.len, n.len);
      }
      if (h.elem && n.elem && !h.elem->num.empty() && !n.elem->num.empty()) {
        Val el = *h.elem;
        el.num = meet(el.num, n.elem->num);
        h.elem = share(el);
      }
      h.num.base = h.len.base = 0;
    }
  }

  // An end at the limit of its type that every pass moves on: as worrying as
  // what moves it, or growing with the loop. A sum or a count in a loop
  // without a bound on its passes is trusted, unless the loop is open-ended
  // (see LoopSpec::openEnded): such a loop ends when the program's own data
  // says so, and that data is in memory.
  auto limit = [&](const Bound &moving, double at, bool unbounded, bool additive = false) {
    if (moving.origin == Origin::Trusted || moving.origin == Origin::Input) {
      return Bound{at, moving.origin, moving.why};
    }
    if (unbounded && additive && !spec.openEnded) {
      return Bound{at, Origin::Trusted, nullptr};
    }

    return Bound{at, Origin::Loop, check_.overflowWarnings().reason(Origin::Loop, spec.loc, "", unbounded)};
  };
  auto stepIn = [&](const VarSymbol *var, bool length) -> std::optional<std::pair<double, double>> {
    auto end = lastEnd.vars.find(var), from = lastHead.vars.find(var);
    if (lastEnd.dead || end == lastEnd.vars.end() || from == lastHead.vars.end()) {
      return std::nullopt;
    }

    return length ? stepOf(end->second.len, from->second.len.base)
                  : stepOf(end->second.num, from->second.num.base);
  };

  // 3. How many passes the loop makes, and so how far what it adds to on
  //    every pass gets.
  Num passes = passCount(spec, entry, head, lastHead, lastEnd);
  // A loop that its checks end after a few passes, as
  // `while (true) { i++; if (i == 3) break }`: run from the entry, it stops.
  if ((passes.empty() || passes.hi > kUnrollPasses) && spec.simple && !check_.overBudget()) {
    State at = entry;
    for (double k = 0; k < kUnrollPasses; ++k) {
      State exits = deadState();
      at = onePass(spec, at, exits);
      if (at.dead) {
        passes = range(0, k + 1);
        break;
      }
    }
  }
  if (!passes.empty() && passes.hi <= kUnrollPasses && spec.simple && !check_.overBudget()) {
    // Few passes: run them one by one.
    State at = entry;
    State all = entry;
    for (double k = 0; k <= passes.hi && !at.dead; ++k) {
      State exits = deadState();
      at = onePass(spec, at, exits);
      all = join(all, at);
    }
    for (auto &headVar : head.vars) {
      auto found = all.vars.find(headVar.first);
      if (found == all.vars.end()) {
        continue;
      }
      Val v = found->second;
      v.num.base = v.len.base = 0;
      // Beyond the type: it wraps around. That is this loop's doing unless
      // the value was at the end of its range when the loop started.
      Limits l = limitsOf(headVar.first->type);
      const Num &start = entry.vars.lookup(headVar.first).num;
      if (!v.num.empty() && v.num.hi > l.hi) {
        v.num.setHigh(start.empty() || start.hi < l.hi ? limit(v.num.high(), l.hi, false)
                                                       : start.high());
      }
      if (!v.num.empty() && v.num.lo < l.lo) {
        v.num.setLow(start.empty() || start.lo > l.lo ? limit(v.num.low(), l.lo, false)
                                                      : start.low());
      }
      headVar.second = v;
    }
  } else {
    for (auto &headVar : head.vars) {
      const VarSymbol *var = headVar.first;
      auto end = lastEnd.vars.find(var);
      if (lastEnd.dead || end == lastEnd.vars.end()) {
        continue;
      }
      Val &h = headVar.second;
      const Val &start = entry.vars.lookup(var);
      auto grow = [&](Num &n, const Num &startNum, const Num &endNum,
                      std::optional<std::pair<double, double>> step, Limits l) {
        if (n.empty() || startNum.empty() || endNum.empty()) {
          return;
        }
        if (n.hi >= l.hi && startNum.hi < l.hi) {
          if (step && !passes.empty() && step->second > 0) {
            // A sum of a bounded number of steps.
            // After k passes the head holds the start plus k steps, and the
            // passes bound the states at the head, the start among them.
            double hi = plus(startNum.hi, times(std::max(passes.hi - 1, 0.0), step->second), kInf);
            if (hi <= l.hi) {
              n.setHigh(joint(hi, joint(hi, startNum.high(), endNum.high()), passes.high()));
            } else {
              n.setHigh(limit(joint(hi, endNum.high(), passes.high()), l.hi, false));
            }
          } else {
            n.setHigh(limit(endNum.high(), l.hi, passes.empty(), step && step->second > 0));
          }
        }
        if (n.lo <= l.lo && startNum.lo > l.lo) {
          if (step && !passes.empty() && step->first < 0) {
            double lo = plus(startNum.lo, times(std::max(passes.hi - 1, 0.0), step->first), -kInf);
            if (lo >= l.lo) {
              n.setLow(joint(lo, joint(lo, startNum.low(), endNum.low()), passes.high()));
            } else {
              n.setLow(limit(joint(lo, endNum.low(), passes.high()), l.lo, false));
            }
          } else {
            n.setLow(limit(endNum.low(), l.lo, passes.empty(), step && step->first < 0));
          }
        }
      };
      grow(h.num, start.num, end->second.num, stepIn(var, false), limitsOf(var->type));
      grow(h.len, start.len, end->second.len, stepIn(var, true), {0, kLengthMax});
      // Elements made from elements, as `a.push(a[i] + 1)`.
      if (h.elem && start.elem && end->second.elem) {
        const Type *element =
            var->type->nonOptional()->isArray() ? var->type->nonOptional()->inner() : nullptr;
        Limits l = limitsOf(element);
        Val el = *h.elem;
        const Num &was = start.elem->num, &moved = end->second.elem->num;
        if (!el.num.empty() && !was.empty() && !moved.empty()) {
          if (el.num.hi >= l.hi && was.hi < l.hi) {
            el.num.setHigh(limit(moved.high(), l.hi, passes.empty()));
          }
          if (el.num.lo <= l.lo && was.lo > l.lo) {
            el.num.setLow(limit(moved.low(), l.lo, passes.empty()));
          }
          h.elem = share(el);
        }
      }
    }
  }

  // What only grew keeps its relation to an outer loop's head; what did not
  // change is exactly what it was.
  for (auto &headVar : head.vars) {
    const Val &e = entry.vars.lookup(headVar.first);
    auto relate = [](Num &n, const Num &start, std::optional<std::pair<double, double>> step) {
      if (!step || n.empty() || start.empty()) {
        return;
      }
      if (step->first == 0 && step->second == 0) {
        n = start;
        return;
      }
      if (!start.base) {
        return;
      }
      n.base = start.base;
      n.offLo = plus(start.offLo, n.lo - start.lo, -kInf);
      n.offHi = plus(start.offHi, n.hi - start.hi, kInf);
    };
    relate(headVar.second.num, e.num, stepIn(headVar.first, false));
    relate(headVar.second.len, e.len, stepIn(headVar.first, true));
  }
  --check_.quiet_;

  // 4. The pass that counts, from the head that holds on every pass.
  State exits = deadState();
  onePass(spec, head, exits);
  st = exits;
}

} // namespace tz
