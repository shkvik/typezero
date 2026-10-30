#include "compiler/overflow/narrowing/narrowing.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/facts/facts.h"
#include "compiler/overflow/call_ranges/call_ranges.h"
#include "compiler/overflow/evaluator/evaluator.h"
#include "compiler/overflow/executor/executor.h"
#include "compiler/overflow/loop_ranges/loop_ranges.h"
#include "compiler/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/overflow/program_scan/program_scan.h"
#include "compiler/overflow/slots/slots.h"
#include "compiler/overflow/value_range/value_range.h"
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

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

std::optional<double> literalValue(const Expr *expr) {
  expr = stripCasts(expr);
  if (auto *lit = dyn_cast_or_null<IntLitExpr>(expr)) {
    return double(lit->value);
  }
  if (auto *lit = dyn_cast_or_null<FloatLitExpr>(expr)) {
    return lit->value;
  }
  if (auto *unary = dyn_cast_or_null<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg) {
    if (std::optional<double> v = literalValue(unary->operand)) {
      return -*v;
    }
  }

  return std::nullopt;
}

}

Narrowing::Narrowing(OverflowCheck &check) : check_(check) {}

void Narrowing::notePredicate(const FunctionDecl *fn, const State &whenTrue,
                              const State &whenFalse) {
  if (check_.quiet_) {
    return;
  }
  OverflowCheck::Pred &pred = check_.next_.preds[fn];
  pred.whenTrue.resize(fn->params.size());
  pred.whenFalse.resize(fn->params.size());
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const VarSymbol *p = fn->params[i].sym;
    if (!p || !isNumberType(p->type) || p->reassigned) {
      continue;
    }
    auto value = [&](const State &s) {
      if (s.dead) {
        return Num();
      }
      auto found = s.vars.find(p);

      return found == s.vars.end() ? Num() : found->second.num;
    };
    pred.whenTrue[i] = join(pred.whenTrue[i], value(whenTrue));
    pred.whenFalse[i] = join(pred.whenFalse[i], value(whenFalse));
  }
}

std::optional<OverflowCheck::Narrowable> Narrowing::narrowable(const Expr *expr,
                                                               const State &st) const {
  while (auto *conv = dyn_cast_or_null<CastExpr>(expr)) {
    if (!conv->isImplicit() || !conv->operand->type ||
        !conv->operand->type->nonOptional()->isNumeric()) {
      break;
    }
    expr = conv->operand;
  }
  if (!expr) {
    return std::nullopt;
  }
  if (auto *bin = dyn_cast<BinaryExpr>(expr);
      bin && (bin->op == BinaryOp::Add || bin->op == BinaryOp::Sub || bin->op == BinaryOp::Mul)) {
    if (bin->op == BinaryOp::Mul) {
      std::optional<OverflowCheck::Narrowable> a = narrowable(bin->lhs, st),
                                               b = narrowable(bin->rhs, st);
      if (a && b && a->var == b->var && a->length == b->length && a->scale == 1 && a->offset == 0 &&
          b->scale == 1 && b->offset == 0 && !a->square && !b->square) {
        a->square = true;
        return a;
      }
    }
    std::optional<double> c = literalValue(bin->rhs);
    const Expr *other = bin->lhs;
    bool constantFirst = false;
    if (!c && bin->op != BinaryOp::Sub) {
      c = literalValue(bin->lhs);
      other = bin->rhs;
      constantFirst = true;
    } else if (!c && bin->op == BinaryOp::Sub) {
      c = literalValue(bin->lhs);
      other = bin->rhs;
      constantFirst = true;
    }
    if (!c) {
      return std::nullopt;
    }
    std::optional<OverflowCheck::Narrowable> n = narrowable(other, st);
    if (!n || n->square) {
      return std::nullopt;
    }
    if (bin->op == BinaryOp::Add) {
      n->offset += *c;
    } else if (bin->op == BinaryOp::Sub) {
      if (constantFirst) {
        n->scale = -n->scale;
        n->offset = *c - n->offset;
      } else {
        n->offset -= *c;
      }
    } else {
      if (*c == 0) {
        return std::nullopt;
      }
      n->scale *= *c;
      n->offset *= *c;
    }
    n->integer = n->integer && *c == std::trunc(*c);
    return n;
  }
  if (auto *call = dyn_cast<CallExpr>(expr);
      call && call->builtin == Builtin::MathAbs && call->args.size() == 1) {
    std::optional<OverflowCheck::Narrowable> inner = narrowable(call->args[0], st);
    if (!inner || inner->length || inner->square || inner->abs || inner->scale != 1 ||
        inner->offset != 0) {
      return std::nullopt;
    }
    inner->abs = true;
    return inner;
  }
  OverflowCheck::Narrowable n;
  const NameExpr *name = nullptr;
  if (auto *update = dyn_cast<UpdateExpr>(expr)) {
    name = dyn_cast<NameExpr>(update->target);
    if (!update->prefix) {
      n.offset = update->increment ? -1 : 1;
    }
    if (name && name->var) {
      n.integer = isIntegerType(name->var->type);
    }
  } else if (auto *member = dyn_cast<MemberExpr>(expr);
             member && member->name == "length" && hasLength(member->object->type)) {
    name = dyn_cast<NameExpr>(stripCasts(member->object));
    n.length = true;
    n.integer = true;
  } else {
    name = dyn_cast<NameExpr>(expr);
    if (name && name->var) {
      n.integer = isIntegerType(name->var->type);
    }
  }
  if (!name || !name->var || !st.vars.count(name->var)) {
    return std::nullopt;
  }
  auto info = check_.vars_.find(name->var);
  if (info == check_.vars_.end() || info->second.owner != check_.owner_ ||
      info->second.writtenElsewhere) {
    return std::nullopt;
  }
  if (!n.length && !isNumberType(name->var->type)) {
    return std::nullopt;
  }
  n.var = name->var;

  return n;
}

void Narrowing::constrain(State &s, const OverflowCheck::Narrowable &target, const Bound &lower,
                          const Bound &upper) {
  if (s.dead) {
    return;
  }
  auto found = s.vars.find(target.var);
  if (found == s.vars.end()) {
    return;
  }
  Num &n = target.length ? found->second.len : found->second.num;
  if (n.empty()) {
    return;
  }
  auto down = [&](double v) { return target.integer ? std::floor(v) : v; };
  auto up = [&](double v) { return target.integer ? std::ceil(v) : v; };
  if (target.abs) {
    if (upper.v < 0) {
      s.dead = true;
      return;
    }
    if (upper.v != kInf) {
      double r = down(upper.v);
      n = narrowTo(n, Bound{-r, upper.origin, upper.why}, Bound{r, upper.origin, upper.why});
    }
    double gap = up(lower.v);
    if (gap > 0 && !n.empty()) {
      if (n.lo >= 0) {
        n = narrowTo(n, Bound{gap, lower.origin, lower.why}, n.high());
      } else if (n.hi <= 0) {
        n = narrowTo(n, n.low(), Bound{-gap, lower.origin, lower.why});
      } else {
        n.away = std::max(n.away, gap);
        if (-gap < n.lo && n.lo < gap) {
          n.setLow({gap, lower.origin, lower.why});
        } else if (-gap < n.hi && n.hi < gap) {
          n.setHigh({-gap, lower.origin, lower.why});
        }
      }
    }
  } else if (target.square) {
    if (upper.v == kInf) {
      return;
    }
    if (upper.v < 0) {
      s.dead = true;
      return;
    }
    double r = down(std::sqrt(upper.v));
    n = narrowTo(n, Bound{-r, upper.origin, upper.why}, Bound{r, upper.origin, upper.why});
  } else {
    Bound lo = lower, hi = upper;
    lo.v = (lower.v - target.offset) / target.scale;
    hi.v = (upper.v - target.offset) / target.scale;
    if (target.scale < 0) {
      std::swap(lo, hi);
    }
    if (std::isnan(lo.v)) {
      lo.v = -kInf;
    }
    if (std::isnan(hi.v)) {
      hi.v = kInf;
    }
    lo.v = up(lo.v);
    hi.v = down(hi.v);
    n = narrowTo(n, lo, hi);
  }
  if (n.empty()) {
    s.dead = true;
  }
}

void Narrowing::narrowComparison(const BinaryExpr *bin, const Val &l, const Val &r, State &whenTrue,
                                 State &whenFalse) {
  auto apply = [&](const Expr *side, const Num &other, BinaryOp op) {
    std::optional<OverflowCheck::Narrowable> target = narrowable(side, whenTrue);
    if (!target || other.empty()) {
      return;
    }
    bool integer =
        target->integer && target->scale == 1 && target->offset == std::trunc(target->offset);
    auto below = [&](const Bound &b) {
      return Bound{integer ? std::ceil(b.v) - 1 : std::nextafter(b.v, -kInf), b.origin, b.why};
    };
    auto above = [&](const Bound &b) {
      return Bound{integer ? std::floor(b.v) + 1 : std::nextafter(b.v, kInf), b.origin, b.why};
    };
    Bound none{-kInf}, top{kInf};
    auto exclude = [&](State &s) {
      if (s.dead || other.lo != other.hi || target->square || (target->abs && other.lo != 0)) {
        return;
      }
      if (!integer && !(other.lo == 0 && target->scale == 1 && target->offset == 0)) {
        return;
      }
      auto found = s.vars.find(target->var);
      if (found == s.vars.end()) {
        return;
      }
      Num &n = target->length ? found->second.len : found->second.num;
      double c = other.lo - target->offset;
      if (n.empty()) {
        return;
      }
      double step = integer ? 1 : std::numeric_limits<double>::denorm_min();
      if (c == 0 && target->scale == 1 && n.lo < 0 && n.hi > 0) {
        n.away = std::max(n.away, step);
      }
      if (n.lo == c) {
        n.lo += step;
      } else if (n.hi == c) {
        n.hi -= step;
      }
      if (n.empty()) {
        s.dead = true;
      }
    };
    switch (op) {
    case BinaryOp::Lt:
      constrain(whenTrue, *target, none, below(other.high()));
      constrain(whenFalse, *target, other.low(), top);
      break;
    case BinaryOp::Le:
      constrain(whenTrue, *target, none, other.high());
      constrain(whenFalse, *target, above(other.low()), top);
      break;
    case BinaryOp::Gt:
      constrain(whenTrue, *target, above(other.low()), top);
      constrain(whenFalse, *target, none, other.high());
      break;
    case BinaryOp::Ge:
      constrain(whenTrue, *target, other.low(), top);
      constrain(whenFalse, *target, none, below(other.high()));
      break;
    case BinaryOp::Eq:
      constrain(whenTrue, *target, other.low(), other.high());
      exclude(whenFalse);
      break;
    case BinaryOp::Ne:
      constrain(whenFalse, *target, other.low(), other.high());
      exclude(whenTrue);
      break;
    default:
      break;
    }
  };
  auto flip = [](BinaryOp op) {
    switch (op) {
    case BinaryOp::Lt:
      return BinaryOp::Gt;
    case BinaryOp::Le:
      return BinaryOp::Ge;
    case BinaryOp::Gt:
      return BinaryOp::Lt;
    case BinaryOp::Ge:
      return BinaryOp::Le;
    default:
      return op;
    }
  };
  const Num &ln = !l.num.empty() ? l.num : l.len;
  const Num &rn = !r.num.empty() ? r.num : r.len;
  apply(bin->lhs, rn, bin->op);
  apply(bin->rhs, ln, flip(bin->op));
}

void Narrowing::applyPredicate(const CallExpr *call, State &whenTrue, State &whenFalse) {
  const FunctionDecl *fn = call->func ? call->func->decl : nullptr;
  if (!fn) {
    return;
  }
  check_.noteRead(&check_.cur_.preds, fn);
  if (check_.round_ == 0) {
    return;
  }
  auto found = check_.cur_.preds.find(fn);
  if (found == check_.cur_.preds.end()) {
    return;
  }
  const OverflowCheck::Pred &pred = found->second;
  for (size_t i = 0; i < call->args.size() && i < fn->params.size(); ++i) {
    std::optional<OverflowCheck::Narrowable> target = narrowable(call->args[i], whenTrue);
    if (!target || target->length) {
      continue;
    }
    auto narrow = [&](State &s, const std::vector<Num> &facts) {
      if (s.dead || i >= facts.size() || facts[i].empty()) {
        return;
      }
      auto var = s.vars.find(target->var);
      if (var == s.vars.end() || var->second.num.empty()) {
        return;
      }
      var->second.num = narrowTo(var->second.num, facts[i].lo, facts[i].hi);
      if (var->second.num.empty()) {
        s.dead = true;
      }
    };
    narrow(whenTrue, pred.whenTrue);
    narrow(whenFalse, pred.whenFalse);
  }
}

void Narrowing::refine(const Expr *cond, State &st, State &whenTrue, State &whenFalse) {
  if (st.dead) {
    whenTrue = whenFalse = st;
    return;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(cond); unary && unary->op == UnaryOp::Not) {
    refine(unary->operand, st, whenFalse, whenTrue);
    return;
  }
  if (auto *lit = dyn_cast<BoolLitExpr>(cond)) {
    whenTrue = whenFalse = st;
    (lit->value ? whenFalse : whenTrue).dead = true;
    return;
  }
  if (auto *seq = dyn_cast<SequenceExpr>(cond); seq && !seq->exprs.empty()) {
    for (size_t i = 0; i + 1 < seq->exprs.size(); ++i) {
      check_.evaluator().eval(seq->exprs[i], st);
    }
    refine(seq->exprs.back(), st, whenTrue, whenFalse);
    return;
  }
  if (auto *bin = dyn_cast<BinaryExpr>(cond)) {
    switch (bin->op) {
    case BinaryOp::LogAnd: {
      State t1, f1, t2, f2;
      refine(bin->lhs, st, t1, f1);
      refine(bin->rhs, t1, t2, f2);
      whenTrue = t2;
      whenFalse = join(f1, f2);
      st = join(whenTrue, whenFalse);
      return;
    }
    case BinaryOp::LogOr: {
      State t1, f1, t2, f2;
      refine(bin->lhs, st, t1, f1);
      refine(bin->rhs, f1, t2, f2);
      whenTrue = join(t1, t2);
      whenFalse = f2;
      st = join(whenTrue, whenFalse);
      return;
    }
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
    case BinaryOp::Eq:
    case BinaryOp::Ne: {
      Val l = check_.evaluator().eval(bin->lhs, st), r = check_.evaluator().eval(bin->rhs, st);
      whenTrue = whenFalse = st;
      if (isNumberType(bin->lhs->type) || isNumberType(bin->rhs->type) ||
          isa<MemberExpr>(stripCasts(bin->lhs)) || isa<MemberExpr>(stripCasts(bin->rhs))) {
        narrowComparison(bin, l, r, whenTrue, whenFalse);
      }
      return;
    }
    default:
      break;
    }
  }
  if (auto *call = dyn_cast<CallExpr>(cond); call && call->func && call->builtin == Builtin::None) {
    check_.evaluator().eval(cond, st);
    whenTrue = whenFalse = st;
    applyPredicate(call, whenTrue, whenFalse);
    return;
  }
  check_.evaluator().eval(cond, st);
  whenTrue = whenFalse = st;
}

}
