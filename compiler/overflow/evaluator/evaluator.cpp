#include "compiler/overflow/evaluator/evaluator.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/facts/facts.h"
#include "compiler/overflow/assign_evaluator/assign_evaluator.h"
#include "compiler/overflow/call_ranges/call_ranges.h"
#include "compiler/overflow/executor/executor.h"
#include "compiler/overflow/loop_ranges/loop_ranges.h"
#include "compiler/overflow/narrowing/narrowing.h"
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

namespace tz {

using llvm::cast;

using llvm::dyn_cast;

namespace {

Num blamed(Num n) {
  if (n.loOrigin == Origin::Trusted) {
    n.setLow({std::clamp(n.lo, -1.0, 1.0)});
  }
  if (n.hiOrigin == Origin::Trusted) {
    n.setHigh({std::clamp(n.hi, -1.0, 1.0)});
  }
  if (n.lo > n.hi) {
    std::swap(n.lo, n.hi);
  }

  return n;
}

bool sameVariable(const Expr *a, const Expr *b) {
  auto *x = llvm::dyn_cast_or_null<NameExpr>(stripCasts(a));
  auto *y = llvm::dyn_cast_or_null<NameExpr>(stripCasts(b));

  return x && y && x->var && x->var == y->var;
}

}

Evaluator::Evaluator(OverflowCheck &check) : check_(check) {}

Val Evaluator::eval(const Expr *expr, State &st, bool wraps) {
  if (!expr || st.dead) {
    return Val();
  }
  switch (expr->kind()) {
  case Expr::Kind::IntLit:
    return numVal(exactly(double(cast<IntLitExpr>(expr)->value)));
  case Expr::Kind::FloatLit:
    return numVal(exactly(cast<FloatLitExpr>(expr)->value));
  case Expr::Kind::BoolLit:
  case Expr::Kind::Undefined:
  case Expr::Kind::RegexLit:
    return Val();
  case Expr::Kind::StringLit:
    return lengthVal(exactly(double(cast<StringLitExpr>(expr)->value.size())));
  case Expr::Kind::Template:
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs) {
      eval(e, st);
    }
    return lengthVal(someLength());
  case Expr::Kind::ArrayLit: {
    auto *lit = cast<ArrayLitExpr>(expr);
    Val element;
    double count = 0;
    bool spreads = false;
    for (const ArrayLitExpr::Element &e : lit->elements) {
      Val v = eval(e.expr, st);
      if (e.spread) {
        spreads = true;
        const Type *type = e.expr->type ? e.expr->type->nonOptional() : nullptr;
        element = join(element,
                       type && type->isArray()
                           ? check_.slots().elementsOf(e.expr, v, type)
                           : check_.overflowWarnings().trusted(
                                 lit->type && lit->type->isArray() ? lit->type->inner() : nullptr));
      } else {
        element = join(element, v);
        ++count;
      }
    }
    Val v;
    v.len = spreads ? make({count}, {kLengthMax, Origin::Trusted}) : exactly(count);
    v.elem = share(element);
    return v;
  }
  case Expr::Kind::ObjectLit:
    return check_.assignEvaluator().evalObjectLit(cast<ObjectLitExpr>(expr), st);
  case Expr::Kind::Name:
    return evalName(cast<NameExpr>(expr), st);
  case Expr::Kind::Member:
    return evalMember(cast<MemberExpr>(expr), st);
  case Expr::Kind::Index:
    return evalIndex(cast<IndexExpr>(expr), st);
  case Expr::Kind::Unary:
    return evalUnary(cast<UnaryExpr>(expr), st, wraps);
  case Expr::Kind::Binary:
    return evalBinary(cast<BinaryExpr>(expr), st, wraps);
  case Expr::Kind::Assign:
    return check_.assignEvaluator().evalAssign(cast<AssignExpr>(expr), st);
  case Expr::Kind::Update:
    return check_.assignEvaluator().evalUpdate(cast<UpdateExpr>(expr), st);
  case Expr::Kind::Call:
    return check_.callRanges().evalCall(cast<CallExpr>(expr), st);
  case Expr::Kind::New:
    return check_.callRanges().evalNew(cast<NewExpr>(expr), st);
  case Expr::Kind::Conditional: {
    auto *c = cast<ConditionalExpr>(expr);
    State t, f;
    check_.narrowing().refine(c->cond, st, t, f);
    Val a = eval(c->thenExpr, t, wraps), b = eval(c->elseExpr, f, wraps);
    st = join(t, f);
    if (t.dead) {
      return b;
    }
    if (f.dead) {
      return a;
    }
    return join(a, b);
  }
  case Expr::Kind::Cast:
    return evalCast(cast<CastExpr>(expr), st, wraps);
  case Expr::Kind::Sequence: {
    Val last;
    for (const Expr *e : cast<SequenceExpr>(expr)->exprs) {
      last = eval(e, st, wraps);
    }
    return last;
  }
  case Expr::Kind::OptionalChain:
    return eval(cast<OptionalChainExpr>(expr)->chain, st, wraps);
  case Expr::Kind::Function:
    return Val();
  case Expr::Kind::Yield:
    eval(cast<YieldExpr>(expr)->value, st);
    check_.callRanges().calledUserCode(st);
    return check_.overflowWarnings().trusted(expr->type);
  case Expr::Kind::Await:
    eval(cast<AwaitExpr>(expr)->value, st);
    check_.callRanges().calledUserCode(st);
    return check_.overflowWarnings().trusted(expr->type);
  }

  return Val();
}

Val Evaluator::evalName(const NameExpr *name, State &st) {
  if (!name->var || !tracked(name->var->type)) {
    return Val();
  }

  return check_.overflowWarnings().fitVal(check_.slots().readVar(name->var, st),
                                          name->type ? name->type : name->var->type);
}

Val Evaluator::evalMember(const MemberExpr *member, State &st) {
  Val object = eval(member->object, st);
  const Type *objectType = member->object->type ? member->object->type->nonOptional() : nullptr;
  if (!objectType) {
    return Val();
  }
  if (member->name == "length" && hasLength(objectType)) {
    return numVal(object.len.empty() ? someLength() : object.len);
  }
  if (objectType->isResult() && member->name == "value" && object.elem) {
    return *object.elem;
  }
  if (!tracked(member->type)) {
    return Val();
  }
  if (objectType->isObject() && !objectType->isAnyObject()) {
    return check_.slots().readField(objectType, member->name, member->type);
  }

  return check_.overflowWarnings().trusted(member->type);
}

Val Evaluator::evalIndex(const IndexExpr *index, State &st) {
  Val object = eval(index->object, st);
  eval(index->index, st);
  const Type *objectType = index->object->type ? index->object->type->nonOptional() : nullptr;
  if (!objectType || !tracked(index->type)) {
    return Val();
  }
  if (objectType->isArray()) {
    return check_.slots().elementsOf(index->object, object, objectType);
  }
  if (objectType->isString()) {
    return lengthVal(range(0, 1));
  }
  if (!index->field.empty() && objectType->isObject()) {
    return check_.slots().readField(objectType, index->field, index->type);
  }

  return check_.overflowWarnings().trusted(index->type);
}

Val Evaluator::evalUnary(const UnaryExpr *unary, State &st, bool wraps) {
  const Type *type = unary->type;
  switch (unary->op) {
  case UnaryOp::Neg: {
    if (auto *lit = dyn_cast<IntLitExpr>(unary->operand)) {
      return numVal(exactly(-double(lit->value)));
    }
    Num a = eval(unary->operand, st, wraps).num;
    if (!isNumberType(type)) {
      return Val();
    }
    return numVal(arithmetic(unary, BinaryOp::Sub, exactly(0), a, type, wraps, nullptr,
                             unary->operand, nullptr));
  }
  case UnaryOp::Plus: {
    Val v = eval(unary->operand, st, wraps);
    const Type *from = unary->operand->type;
    if (from && from->nonOptional()->isString()) {
      return numVal(check_.overflowWarnings().input(type, unary->loc(), "'+' of a string"));
    }
    return numVal(v.num);
  }
  case UnaryOp::BitNot: {
    Num a = eval(unary->operand, st, true).num;
    if (a.empty() || !isNumberType(type)) {
      return Val();
    }
    Num r = make({-a.hi - 1, a.hiOrigin, a.hiWhy}, {-a.lo - 1, a.loOrigin, a.loWhy});
    return numVal(check_.overflowWarnings().fit(r, type));
  }
  default:
    eval(unary->operand, st);
    return Val();
  }
}

Num Evaluator::bitwise(BinaryOp op, const Num &l, const Num &r, const Type *type) const {
  if (l.empty() || r.empty()) {
    return Num();
  }
  Limits lim = limitsOf(type);
  unsigned width = type && type->nonOptional()->isInteger() ? type->nonOptional()->bitWidth() : 64;
  Num full = mixed(lim.lo, lim.hi, l, r);
  bool lPos = l.lo >= 0, rPos = r.lo >= 0;
  auto zero = Bound{0};
  switch (op) {
  case BinaryOp::BitAnd:
    if (lPos && rPos) {
      return make(zero, l.hi < r.hi ? l.high() : r.high());
    }
    if (rPos) {
      return make(zero, r.high());
    }
    if (lPos) {
      return make(zero, l.high());
    }
    return full;
  case BinaryOp::BitOr:
    if (r.lo == 0 && r.hi == 0) {
      return check_.overflowWarnings().fit(l, type);
    }
    if (l.lo == 0 && l.hi == 0) {
      return check_.overflowWarnings().fit(r, type);
    }
    if (lPos && rPos) {
      const Bound &top = l.hi > r.hi ? l.high() : r.high();
      return make(l.lo > r.lo ? l.low() : r.low(), {ones(top.v), top.origin, top.why});
    }
    return full;
  case BinaryOp::BitXor:
    if (lPos && rPos) {
      const Bound &top = l.hi > r.hi ? l.high() : r.high();
      return make(zero, {ones(top.v), top.origin, top.why});
    }
    return full;
  case BinaryOp::Shl:
    if (r.lo == r.hi && r.lo >= 0 && r.lo < width && lPos) {
      double k = std::ldexp(1.0, int(r.lo));
      if (l.hi * k <= lim.hi) {
        return make({l.lo * k, l.loOrigin, l.loWhy}, {l.hi * k, l.hiOrigin, l.hiWhy});
      }
    }
    return full;
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    if (r.lo >= 0 && r.hi < width) {
      double near = std::ldexp(1.0, int(r.lo)), far = std::ldexp(1.0, int(r.hi));
      double lo = l.lo < 0 ? std::floor(l.lo / near) : std::floor(l.lo / far);
      double hi = l.hi < 0 ? std::floor(l.hi / far) : std::floor(l.hi / near);
      return make({lo, l.loOrigin, l.loWhy}, {hi, l.hiOrigin, l.hiWhy});
    }
    return make(l.lo < 0 ? l.low() : zero, l.hi > 0 ? l.high() : zero);
  }
  default:
    return full;
  }
}

Val Evaluator::evalBinary(const BinaryExpr *bin, State &st, bool wraps) {
  switch (bin->op) {
  case BinaryOp::LogAnd:
  case BinaryOp::LogOr: {
    State t, f;
    check_.narrowing().refine(bin, st, t, f);
    return check_.overflowWarnings().trusted(bin->type);
  }
  case BinaryOp::Nullish: {
    Val a = eval(bin->lhs, st, wraps);
    State other = st;
    Val b = eval(bin->rhs, other, wraps);
    st = join(st, other);
    return join(a, b);
  }
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge:
  case BinaryOp::Eq:
  case BinaryOp::Ne:
  case BinaryOp::In:
  case BinaryOp::InstanceOf:
    eval(bin->lhs, st);
    eval(bin->rhs, st);
    return Val();
  default:
    break;
  }
  bool bits = bin->op == BinaryOp::BitAnd || bin->op == BinaryOp::BitOr ||
              bin->op == BinaryOp::BitXor || bin->op == BinaryOp::Shl || bin->op == BinaryOp::Shr ||
              bin->op == BinaryOp::UShr;
  bool operandsWrap = false;
  Val l = eval(bin->lhs, st, operandsWrap);
  Val r = eval(bin->rhs, st, operandsWrap);
  const Type *type = bin->type;
  if (type && type->isString()) {
    if (l.len.empty() || r.len.empty()) {
      return lengthVal(someLength());
    }
    return lengthVal(narrowTo(add(l.len, r.len), 0, kLengthMax));
  }
  if (!isNumberType(type)) {
    return Val();
  }
  bool shift = bin->op == BinaryOp::Shl || bin->op == BinaryOp::Shr || bin->op == BinaryOp::UShr;
  const Type *base = type->nonOptional();
  if (base->isInteger() && base->isWrapping() && bin->op == BinaryOp::Shl) {
    Limits whole = limitsOf(type);
    return numVal(range(whole.lo, whole.hi));
  }
  if (shift && base->isInteger() && base->isWrapping()) {
    return numVal(bitwise(bin->op, l.num, r.num, type));
  }
  if (shift && base->isInteger()) {
    check_.overflowWarnings().checkShiftCount(bin->rhs, r.num, base->bitWidth());
  }
  if (bin->op == BinaryOp::Shl && base->isInteger() && !l.num.empty() && !r.num.empty() &&
      r.num.lo >= 0 && r.num.hi < base->bitWidth()) {
    Num factor = make({std::ldexp(1.0, int(r.num.lo)), r.num.loOrigin, r.num.loWhy},
                      {std::ldexp(1.0, int(r.num.hi)), r.num.hiOrigin, r.num.hiWhy});
    return numVal(
        arithmetic(bin, BinaryOp::Mul, l.num, factor, type, false, bin->lhs, bin->rhs, nullptr));
  }
  if (bits) {
    if (base->isWrapping() && (l.num.empty() || r.num.empty())) {
      Limits whole = limitsOf(type);
      return numVal(range(whole.lo, whole.hi));
    }
    return numVal(bitwise(bin->op, l.num, r.num, type));
  }
  const VarSymbol *accum = check_.selfAssigned_.lookup(bin);
  if (auto *name = dyn_cast<NameExpr>(stripCasts(bin->lhs));
      !accum && name && check_.accumulators_.contains(name->var)) {
    accum = name->var;
  }

  bool square = bin->op == BinaryOp::Mul && sameVariable(bin->lhs, bin->rhs);

  return numVal(
      arithmetic(bin, bin->op, l.num, r.num, type, wraps, bin->lhs, bin->rhs, accum, square));
}

Num Evaluator::floatArithmetic(const Expr *site, BinaryOp op, const Num &l, const Num &r,
                               const Type *type, const Expr *lhs, const Expr *rhs,
                               const VarSymbol *accum, bool square) {
  OverflowWarnings &warnings = check_.overflowWarnings();
  Limits lim = limitsOf(type);
  if (l.empty() || r.empty()) {
    warnings.noteProof(site, false);
    return Num();
  }
  bool ok = true;
  if (op == BinaryOp::Div || op == BinaryOp::Rem) {
    ok = warnings.checkDivisor(site, op, rhs, l, r);
  } else if (op == BinaryOp::Pow) {
    ok = warnings.checkDivisor(site, op, lhs, l, r);
    const Expr *exponent = stripCasts(rhs);
    bool integerExponent =
        (exponent && isIntegerType(exponent->type)) || (r.lo == r.hi && std::trunc(r.lo) == r.lo);
    ok = ok && (integerExponent || warnings.checkPowerBase(site, lhs, l));
  }
  auto apply = [&](const Num &a, const Num &b) {
    switch (op) {
    case BinaryOp::Add:
      return add(a, b);
    case BinaryOp::Sub:
      return subtract(a, b);
    case BinaryOp::Mul:
      return square ? squared(a) : multiply(a, b);
    case BinaryOp::Div:
      return divide(a, b, false);
    case BinaryOp::Rem:
      return remainder(a, b, false);
    case BinaryOp::Pow:
      return powerFloat(a, b);
    default:
      return range(-kInf, kInf);
    }
  };
  if (!ok) {
    warnings.noteProof(site, false);
    return range(lim.lo, lim.hi, Origin::Trusted);
  }
  Num exact = apply(l, r);
  if (!exact.empty() && fits(exact, type)) {
    warnings.noteProof(site, provenFinite(l) && provenFinite(r));
    return exact;
  }
  warnings.noteProof(site, false);
  if (check_.quiet_) {
    return exact;
  }
  Num exceeding = apply(blamed(l), blamed(r));
  if (!exceeding.empty() && !fits(exceeding, type)) {
    warnings.warn(site, l, r, lhs, rhs, exceeding, accum, type);
    return range(lim.lo, lim.hi, Origin::Trusted);
  }
  if (exact.empty()) {
    return range(lim.lo, lim.hi, Origin::Trusted);
  }
  if (exact.lo < lim.lo) {
    exact.setLow({lim.lo, Origin::Trusted});
  }
  if (exact.hi > lim.hi) {
    exact.setHigh({lim.hi, Origin::Trusted});
  }

  return exact;
}

Num Evaluator::arithmetic(const Expr *site, BinaryOp op, const Num &given, const Num &other,
                          const Type *type, bool wraps, const Expr *lhs, const Expr *rhs,
                          const VarSymbol *accum, bool square) {
  if (isFloatType(type)) {
    return floatArithmetic(site, op, given, other, type, lhs, rhs, accum, square);
  }
  bool integer = isIntegerType(type);
  if (integer && type->nonOptional()->isWrapping()) {
    Limits whole = limitsOf(type);
    bool divides = op == BinaryOp::Div || op == BinaryOp::Rem;
    if (divides && !given.empty() && !other.empty()) {
      check_.overflowWarnings().noteProof(
          site, check_.overflowWarnings().checkDivisor(site, op, rhs, given, other));
    }
    return range(whole.lo, whole.hi);
  }
  bool narrow = integer && type->nonOptional()->isInteger() && type->nonOptional()->bitWidth() < 64;
  Num l = given, r = other;
  if (narrow) {
    Limits whole = limitsOf(type);
    if (l.empty()) {
      l = range(whole.lo, whole.hi, Origin::Trusted);
    }
    if (r.empty() && op != BinaryOp::Pow) {
      r = range(whole.lo, whole.hi, Origin::Trusted);
    }
  }
  if (l.empty() || r.empty()) {
    return Num();
  }
  auto apply = [&](const Num &a, const Num &b) {
    switch (op) {
    case BinaryOp::Add:
      return add(a, b);
    case BinaryOp::Sub:
      return subtract(a, b);
    case BinaryOp::Mul:
      return square ? squared(a) : multiply(a, b);
    case BinaryOp::Div:
      return divide(a, b, integer);
    case BinaryOp::Rem:
      return remainder(a, b);
    case BinaryOp::Pow:
      return power(a, b);
    default:
      return range(-kInf, kInf);
    }
  };
  bool divides =
      integer && (op == BinaryOp::Div || op == BinaryOp::Rem || (op == BinaryOp::Pow && r.lo < 0));
  bool divisorOk = !divides || check_.overflowWarnings().checkDivisor(
                                   site, op, op == BinaryOp::Pow ? lhs : rhs, l, r);
  Num exact = apply(l, r);
  if (divides) {
    bool minByMinusOne =
        !type->nonOptional()->isUnsigned() && r.lo <= -1 && r.hi >= -1 && l.lo <= limitsOf(type).lo;
    check_.overflowWarnings().noteProof(site, divisorOk && !minByMinusOne && !exact.empty() &&
                                                  fits(exact, type));
  }
  if (exact.empty() || !type->nonOptional()->isInteger() || op == BinaryOp::Rem) {
    return check_.overflowWarnings().fit(exact, type);
  }
  Limits lim = limitsOf(type);
  if (fits(exact, type)) {
    return exact;
  }
  if (!wraps && !check_.quiet_) {
    auto suspect = [&](const Num &n) { return narrow ? n : blamed(n); };
    Num exceeding = apply(suspect(l), suspect(r));
    bool constant = (!lhs || check_.programScan().isConstantTree(lhs)) &&
                    (!rhs || check_.programScan().isConstantTree(rhs)) && (lhs || rhs);
    if (!exceeding.empty() && !fits(exceeding, type) && !constant) {
      check_.overflowWarnings().warn(site, l, r, lhs, rhs, exceeding, accum, type);
      return range(lim.lo, lim.hi, Origin::Trusted);
    }
  }
  if (check_.quiet_) {
    return exact;
  }

  return range(lim.lo, lim.hi, wraps ? Origin::Proven : Origin::Trusted);
}

Val Evaluator::evalCast(const CastExpr *conv, State &st, bool wraps) {
  Val v = eval(conv->operand, st, wraps);
  const Type *from = conv->operand->type, *to = conv->type;
  if (!to || !from) {
    return Val();
  }
  if (to->isString()) {
    return lengthVal(someLength());
  }
  if (from->nonOptional()->isJson() && isNumberType(to)) {
    return numVal(check_.overflowWarnings().input(to, conv->loc(), "JSON"));
  }
  if (!isNumberType(to)) {
    return hasLength(to) || (from->isResult() && to->isResult()) ? v : Val();
  }
  if (!isNumberType(from)) {
    return check_.overflowWarnings().trusted(to);
  }
  Num n = v.num;
  if (n.empty()) {
    return Val();
  }
  if (isIntegerType(to) && !isIntegerType(from)) {
    Limits l = limitsOf(to);
    if (n.lo >= l.lo && n.hi <= l.hi) {
      return numVal(
          make({std::trunc(n.lo), n.loOrigin, n.loWhy}, {std::trunc(n.hi), n.hiOrigin, n.hiWhy}));
    }
    Bound w = joint(0, n.low(), n.high());
    return numVal(range(l.lo, l.hi, w.origin, w.why));
  }

  return numVal(check_.overflowWarnings().fit(n, to));
}

}
