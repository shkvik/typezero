#include "compiler/interprocedural/overflow/evaluator/evaluator.h"
#include "compiler/interprocedural/overflow/assign_evaluator/assign_evaluator.h"
#include "compiler/interprocedural/overflow/call_ranges/call_ranges.h"
#include "compiler/interprocedural/overflow/executor/executor.h"
#include "compiler/interprocedural/overflow/loop_ranges/loop_ranges.h"
#include "compiler/interprocedural/overflow/narrowing/narrowing.h"
#include "compiler/interprocedural/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/interprocedural/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/interprocedural/overflow/program_scan/program_scan.h"
#include "compiler/interprocedural/overflow/slots/slots.h"
#include "compiler/interprocedural/overflow/value_range/value_range.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/decimal/decimal_literal/decimal_literal.h"
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

Evaluator::Evaluator(OverflowCheck &check)
    : check(check) {}

Num Evaluator::blamed(Num n) const {
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

bool Evaluator::sameVariable(const Expr *a, const Expr *b) const {
  auto *x = llvm::dyn_cast_or_null<NameExpr>(OverflowHelpers::stripCasts(a));
  auto *y = llvm::dyn_cast_or_null<NameExpr>(OverflowHelpers::stripCasts(b));

  return x && y && x->var() && x->var() == y->var();
}

Val Evaluator::eval(const Expr *expr, State &st, bool wraps) {
  if (!expr || st.dead) {
    return Val();
  }
  switch (expr->kind()) {
  case Expr::Kind::IntLit:
    if (ValueRange::isDecimalType(expr->type)) {
      return ValueRange::numVal(decimalLiteral(expr));
    }
    return ValueRange::numVal(ValueRange::exactly(double(cast<IntLitExpr>(expr)->value)));
  case Expr::Kind::FloatLit:
    if (ValueRange::isDecimalType(expr->type)) {
      return ValueRange::numVal(decimalLiteral(expr));
    }
    return ValueRange::numVal(ValueRange::exactly(cast<FloatLitExpr>(expr)->value));
  case Expr::Kind::BoolLit:
  case Expr::Kind::Undefined:
  case Expr::Kind::RegexLit:
    return Val();
  case Expr::Kind::StringLit:
    return ValueRange::lengthVal(
        ValueRange::exactly(double(cast<StringLitExpr>(expr)->value.size())));
  case Expr::Kind::Template:
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs) {
      eval(e, st);
    }
    return ValueRange::lengthVal(ValueRange::someLength());
  case Expr::Kind::ArrayLit: {
    auto *lit = cast<ArrayLitExpr>(expr);
    Val element;
    double count = 0;
    bool isSpreading = false;
    for (const ArrayLitExpr::Element &e : lit->elements) {
      Val v = eval(e.expr, st);
      if (e.spread) {
        isSpreading = true;
        const Type *type = e.expr->type ? e.expr->type->nonOptional() : nullptr;
        element = ValueRange::join(
            element, type && type->isArray()
                         ? this->check.slots().elementsOf(e.expr, v, type)
                         : this->check.overflowWarnings().trusted(
                               lit->type && lit->type->isArray() ? lit->type->inner() : nullptr));
      } else {
        element = ValueRange::join(element, v);
        ++count;
      }
    }
    Val v;
    v.len = isSpreading ? ValueRange::make({count}, {kLengthMax, Origin::Trusted})
                        : ValueRange::exactly(count);
    v.elem = ValueRange::share(element);
    return v;
  }
  case Expr::Kind::ObjectLit:
    return this->check.assignEvaluator().evalObjectLit(cast<ObjectLitExpr>(expr), st);
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
    return this->check.assignEvaluator().evalAssign(cast<AssignExpr>(expr), st);
  case Expr::Kind::Update:
    return this->check.assignEvaluator().evalUpdate(cast<UpdateExpr>(expr), st);
  case Expr::Kind::Call:
    return this->check.callRanges().evalCall(cast<CallExpr>(expr), st);
  case Expr::Kind::New:
    return this->check.callRanges().evalNew(cast<NewExpr>(expr), st);
  case Expr::Kind::Conditional: {
    auto *c = cast<ConditionalExpr>(expr);
    State t, f;
    this->check.narrowing().refine(c->cond, st, t, f);
    Val a = eval(c->thenExpr, t, wraps), b = eval(c->elseExpr, f, wraps);
    st = ValueRange::join(t, f);
    if (t.dead) {
      return b;
    }
    if (f.dead) {
      return a;
    }
    return ValueRange::join(a, b);
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
    this->check.callRanges().calledUserCode(st);
    return this->check.overflowWarnings().trusted(expr->type);
  case Expr::Kind::Await:
    eval(cast<AwaitExpr>(expr)->value, st);
    this->check.callRanges().calledUserCode(st);
    return this->check.overflowWarnings().trusted(expr->type);
  case Expr::Kind::Class:
    break;
  }

  return Val();
}

Val Evaluator::evalName(const NameExpr *name, State &st) {
  if (!name->var() || !ValueRange::tracked(name->var()->type)) {
    return Val();
  }

  return this->check.overflowWarnings().fitVal(this->check.slots().readVar(name->var(), st),
                                               name->type ? name->type : name->var()->type);
}

Val Evaluator::evalMember(const MemberExpr *member, State &st) {
  Val object = eval(member->object, st);
  const Type *objectType = member->object->type ? member->object->type->nonOptional() : nullptr;
  if (!objectType) {
    return Val();
  }
  if (BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH) &&
      ValueRange::hasLength(objectType)) {
    return ValueRange::numVal(object.len.empty() ? ValueRange::someLength() : object.len);
  }
  if (objectType->isResult() && member->name == "value" && object.elem) {
    return *object.elem;
  }
  if (!ValueRange::tracked(member->type)) {
    return Val();
  }
  if (objectType->isObject() && !objectType->isAnyObject()) {
    return this->check.slots().readField(objectType, member->name, member->type);
  }

  return this->check.overflowWarnings().trusted(member->type);
}

Val Evaluator::evalIndex(const IndexExpr *index, State &st) {
  Val object = eval(index->object, st);
  eval(index->index, st);
  const Type *objectType = index->object->type ? index->object->type->nonOptional() : nullptr;
  if (!objectType || !ValueRange::tracked(index->type)) {
    return Val();
  }
  if (objectType->isArray()) {
    return this->check.slots().elementsOf(index->object, object, objectType);
  }
  if (objectType->isString()) {
    return ValueRange::lengthVal(ValueRange::range(0, 1));
  }
  if (!index->field().empty() && objectType->isObject()) {
    return this->check.slots().readField(objectType, index->field(), index->type);
  }

  return this->check.overflowWarnings().trusted(index->type);
}

Val Evaluator::evalUnary(const UnaryExpr *unary, State &st, bool wraps) {
  const Type *type = unary->type;
  switch (unary->op) {
  case UnaryOp::Neg: {
    if (auto *lit = dyn_cast<IntLitExpr>(unary->operand); lit && !ValueRange::isDecimalType(type)) {
      return ValueRange::numVal(ValueRange::exactly(-double(lit->value)));
    }
    Num a = eval(unary->operand, st, wraps).num;
    if (!ValueRange::isNumberType(type)) {
      return Val();
    }
    return ValueRange::numVal(arithmetic(unary, BinaryOp::Sub, ValueRange::exactly(0), a, type,
                                         wraps, nullptr, unary->operand, nullptr));
  }
  case UnaryOp::Plus: {
    Val v = eval(unary->operand, st, wraps);
    const Type *from = unary->operand->type;
    if (from && from->nonOptional()->isString()) {
      return ValueRange::numVal(
          this->check.overflowWarnings().input(type, unary->loc(), "'+' of a string"));
    }
    return ValueRange::numVal(v.num);
  }
  case UnaryOp::BitNot: {
    Num a = eval(unary->operand, st, true).num;
    if (a.empty() || !ValueRange::isNumberType(type)) {
      return Val();
    }
    Num r = ValueRange::make({-a.hi - 1, a.hiOrigin, a.hiWhy}, {-a.lo - 1, a.loOrigin, a.loWhy});
    return ValueRange::numVal(this->check.overflowWarnings().fit(r, type));
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
  Limits lim = ValueRange::limitsOf(type);
  unsigned width = type && type->nonOptional()->isInteger() ? type->nonOptional()->bitWidth() : 64;
  Num full = ValueRange::mixed(lim.lo, lim.hi, l, r);
  bool isLPos = l.lo >= 0, rPos = r.lo >= 0;
  auto zero = Bound{0};
  switch (op) {
  case BinaryOp::BitAnd:
    if (isLPos && rPos) {
      return ValueRange::make(zero, l.hi < r.hi ? l.high() : r.high());
    }
    if (rPos) {
      return ValueRange::make(zero, r.high());
    }
    if (isLPos) {
      return ValueRange::make(zero, l.high());
    }
    return full;
  case BinaryOp::BitOr:
    if (r.lo == 0 && r.hi == 0) {
      return this->check.overflowWarnings().fit(l, type);
    }
    if (l.lo == 0 && l.hi == 0) {
      return this->check.overflowWarnings().fit(r, type);
    }
    if (isLPos && rPos) {
      const Bound &top = l.hi > r.hi ? l.high() : r.high();
      return ValueRange::make(l.lo > r.lo ? l.low() : r.low(),
                              {ValueRange::ones(top.v), top.origin, top.why});
    }
    return full;
  case BinaryOp::BitXor:
    if (isLPos && rPos) {
      const Bound &top = l.hi > r.hi ? l.high() : r.high();
      return ValueRange::make(zero, {ValueRange::ones(top.v), top.origin, top.why});
    }
    return full;
  case BinaryOp::Shl:
    if (r.lo == r.hi && r.lo >= 0 && r.lo < width && isLPos) {
      double k = std::ldexp(1.0, int(r.lo));
      if (l.hi * k <= lim.hi) {
        return ValueRange::make({l.lo * k, l.loOrigin, l.loWhy}, {l.hi * k, l.hiOrigin, l.hiWhy});
      }
    }
    return full;
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    if (r.lo >= 0 && r.hi < width) {
      double near = std::ldexp(1.0, int(r.lo)), far = std::ldexp(1.0, int(r.hi));
      double lo = l.lo < 0 ? std::floor(l.lo / near) : std::floor(l.lo / far);
      double hi = l.hi < 0 ? std::floor(l.hi / far) : std::floor(l.hi / near);
      return ValueRange::make({lo, l.loOrigin, l.loWhy}, {hi, l.hiOrigin, l.hiWhy});
    }
    return ValueRange::make(l.lo < 0 ? l.low() : zero, l.hi > 0 ? l.high() : zero);
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
    this->check.narrowing().refine(bin, st, t, f);
    return this->check.overflowWarnings().trusted(bin->type);
  }
  case BinaryOp::Nullish: {
    Val a = eval(bin->lhs, st, wraps);
    State other = st;
    Val b = eval(bin->rhs, other, wraps);
    st = ValueRange::join(st, other);
    return ValueRange::join(a, b);
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
  bool hasBits = bin->op == BinaryOp::BitAnd || bin->op == BinaryOp::BitOr ||
                 bin->op == BinaryOp::BitXor || bin->op == BinaryOp::Shl ||
                 bin->op == BinaryOp::Shr || bin->op == BinaryOp::UShr;
  bool isOperandWrapping = false;
  Val l = eval(bin->lhs, st, isOperandWrapping);
  Val r = eval(bin->rhs, st, isOperandWrapping);
  const Type *type = bin->type;
  if (type && type->isString()) {
    if (l.len.empty() || r.len.empty()) {
      return ValueRange::lengthVal(ValueRange::someLength());
    }
    return ValueRange::lengthVal(
        ValueRange::narrowTo(ValueRange::add(l.len, r.len), 0, kLengthMax));
  }
  if (!ValueRange::isNumberType(type)) {
    return Val();
  }
  bool isShift = bin->op == BinaryOp::Shl || bin->op == BinaryOp::Shr || bin->op == BinaryOp::UShr;
  const Type *base = type->nonOptional();
  if (base->isInteger() && base->isWrapping() && bin->op == BinaryOp::Shl) {
    Limits whole = ValueRange::limitsOf(type);
    return ValueRange::numVal(ValueRange::range(whole.lo, whole.hi));
  }
  if (isShift && base->isInteger() && base->isWrapping()) {
    return ValueRange::numVal(bitwise(bin->op, l.num, r.num, type));
  }
  if (isShift && base->isInteger()) {
    this->check.overflowWarnings().checkShiftCount(bin->rhs, r.num, base->bitWidth());
  }
  if (bin->op == BinaryOp::Shl && base->isInteger() && !l.num.empty() && !r.num.empty() &&
      r.num.lo >= 0 && r.num.hi < base->bitWidth()) {
    Num factor = ValueRange::make({std::ldexp(1.0, int(r.num.lo)), r.num.loOrigin, r.num.loWhy},
                                  {std::ldexp(1.0, int(r.num.hi)), r.num.hiOrigin, r.num.hiWhy});
    return ValueRange::numVal(
        arithmetic(bin, BinaryOp::Mul, l.num, factor, type, false, bin->lhs, bin->rhs, nullptr));
  }
  if (hasBits) {
    if (base->isWrapping() && (l.num.empty() || r.num.empty())) {
      Limits whole = ValueRange::limitsOf(type);
      return ValueRange::numVal(ValueRange::range(whole.lo, whole.hi));
    }
    return ValueRange::numVal(bitwise(bin->op, l.num, r.num, type));
  }
  const VarSymbol *accum = this->check.selfAssigned.lookup(bin);
  if (auto *name = dyn_cast<NameExpr>(OverflowHelpers::stripCasts(bin->lhs));
      !accum && name && this->check.accumulators.contains(name->var())) {
    accum = name->var();
  }

  bool isSquare = bin->op == BinaryOp::Mul && this->sameVariable(bin->lhs, bin->rhs);

  return ValueRange::numVal(
      arithmetic(bin, bin->op, l.num, r.num, type, wraps, bin->lhs, bin->rhs, accum, isSquare));
}

Num Evaluator::floatArithmetic(const Expr *site, BinaryOp op, const Num &l, const Num &r,
                               const Type *type, const Expr *lhs, const Expr *rhs,
                               const VarSymbol *accum, bool square) {
  OverflowWarnings &warnings = this->check.overflowWarnings();
  Limits lim = ValueRange::limitsOf(type);
  if (l.empty() || r.empty()) {
    warnings.noteProof(site, false);
    return Num();
  }
  bool isOk = true;
  if (op == BinaryOp::Div || op == BinaryOp::Rem) {
    isOk = warnings.checkDivisor(site, op, rhs, l, r);
  } else if (op == BinaryOp::Pow) {
    isOk = warnings.checkDivisor(site, op, lhs, l, r);
    const Expr *exponent = OverflowHelpers::stripCasts(rhs);
    bool isIntegerExponent = (exponent && ValueRange::isIntegerType(exponent->type)) ||
                             (r.lo == r.hi && std::trunc(r.lo) == r.lo);
    isOk = isOk && (isIntegerExponent || warnings.checkPowerBase(site, lhs, l));
  }
  auto apply = [&](const Num &a, const Num &b) {
    switch (op) {
    case BinaryOp::Add:
      return ValueRange::add(a, b);
    case BinaryOp::Sub:
      return ValueRange::subtract(a, b);
    case BinaryOp::Mul:
      return square ? ValueRange::squared(a) : ValueRange::multiply(a, b);
    case BinaryOp::Div:
      return ValueRange::divide(a, b, false);
    case BinaryOp::Rem:
      return ValueRange::remainder(a, b, false);
    case BinaryOp::Pow:
      return ValueRange::powerFloat(a, b);
    default:
      return ValueRange::range(-kInf, kInf);
    }
  };
  if (!isOk) {
    warnings.noteProof(site, false);
    return ValueRange::range(lim.lo, lim.hi, Origin::Trusted);
  }
  Num exact = apply(l, r);
  if (!exact.empty() && ValueRange::fits(exact, type)) {
    warnings.noteProof(site, ValueRange::provenFinite(l) && ValueRange::provenFinite(r));
    return exact;
  }
  warnings.noteProof(site, false);
  if (this->check.quiet) {
    return exact;
  }
  Num exceeding = apply(this->blamed(l), this->blamed(r));
  if (!exceeding.empty() && !ValueRange::fits(exceeding, type)) {
    warnings.warn(site, l, r, lhs, rhs, exceeding, accum, type);
    return ValueRange::range(lim.lo, lim.hi, Origin::Trusted);
  }
  if (exact.empty()) {
    return ValueRange::range(lim.lo, lim.hi, Origin::Trusted);
  }
  if (exact.lo < lim.lo) {
    exact.setLow({lim.lo, Origin::Trusted});
  }
  if (exact.hi > lim.hi) {
    exact.setHigh({lim.hi, Origin::Trusted});
  }

  return exact;
}

Num Evaluator::decimalLiteral(const Expr *expr) const {
  std::optional<DecimalLiteral> lit = DecimalLiteral::fromExpr(expr);
  if (!lit) {
    return ValueRange::range(-kInf, kInf, Origin::Trusted);
  }
  if (lit->isZero()) {
    return ValueRange::exactly(0);
  }
  std::string text = lit->digits + "e" + std::to_string(lit->exponent);
  double v = std::strtod(text.c_str(), nullptr);
  if (lit->exponent >= 0 && lit->top() < 15) {
    return ValueRange::exactly(v);
  }

  return ValueRange::widenedDecimal(ValueRange::exactly(v));
}

Num Evaluator::decimalArithmetic(const Expr *site, BinaryOp op, const Num &given, const Num &other,
                                 const Expr *lhs, const Expr *rhs, bool square) {
  OverflowWarnings &warnings = this->check.overflowWarnings();
  Num unknown = ValueRange::range(-kInf, kInf, Origin::Trusted);
  Num l = given.empty() ? unknown : given, r = other.empty() ? unknown : other;
  bool isDividing = op == BinaryOp::Div || op == BinaryOp::Rem || (op == BinaryOp::Pow && r.lo < 0);
  if (isDividing) {
    bool isOk = warnings.checkDivisor(site, op, op == BinaryOp::Pow ? lhs : rhs, l, r);
    warnings.noteProof(site, isOk);
    if (!isOk) {
      return unknown;
    }
  }
  switch (op) {
  case BinaryOp::Add:
    return ValueRange::widenedDecimal(ValueRange::add(l, r));
  case BinaryOp::Sub:
    return ValueRange::widenedDecimal(ValueRange::subtract(l, r));
  case BinaryOp::Mul:
    return ValueRange::widenedDecimal(square ? ValueRange::squared(l) : ValueRange::multiply(l, r));
  case BinaryOp::Div:
    return ValueRange::widenedDecimal(ValueRange::divide(l, r, false), 1e-20);
  case BinaryOp::Rem:
    return ValueRange::widenedDecimal(ValueRange::remainder(l, r, false));
  default:
    return unknown;
  }
}

Num Evaluator::arithmetic(const Expr *site, BinaryOp op, const Num &given, const Num &other,
                          const Type *type, bool wraps, const Expr *lhs, const Expr *rhs,
                          const VarSymbol *accum, bool square) {
  if (ValueRange::isDecimalType(type)) {
    return decimalArithmetic(site, op, given, other, lhs, rhs, square);
  }
  if (ValueRange::isFloatType(type)) {
    return floatArithmetic(site, op, given, other, type, lhs, rhs, accum, square);
  }
  bool isInteger = ValueRange::isIntegerType(type);
  if (isInteger && type->nonOptional()->isWrapping()) {
    Limits whole = ValueRange::limitsOf(type);
    bool isDividing = op == BinaryOp::Div || op == BinaryOp::Rem;
    if (isDividing && !given.empty() && !other.empty()) {
      this->check.overflowWarnings().noteProof(
          site, this->check.overflowWarnings().checkDivisor(site, op, rhs, given, other));
    }
    return ValueRange::range(whole.lo, whole.hi);
  }
  bool isNarrow =
      isInteger && type->nonOptional()->isInteger() && type->nonOptional()->bitWidth() < 64;
  Num l = given, r = other;
  if (isNarrow) {
    Limits whole = ValueRange::limitsOf(type);
    if (l.empty()) {
      l = ValueRange::range(whole.lo, whole.hi, Origin::Trusted);
    }
    if (r.empty() && op != BinaryOp::Pow) {
      r = ValueRange::range(whole.lo, whole.hi, Origin::Trusted);
    }
  }
  if (l.empty() || r.empty()) {
    return Num();
  }
  auto apply = [&](const Num &a, const Num &b) {
    switch (op) {
    case BinaryOp::Add:
      return ValueRange::add(a, b);
    case BinaryOp::Sub:
      return ValueRange::subtract(a, b);
    case BinaryOp::Mul:
      return square ? ValueRange::squared(a) : ValueRange::multiply(a, b);
    case BinaryOp::Div:
      return ValueRange::divide(a, b, isInteger);
    case BinaryOp::Rem:
      return ValueRange::remainder(a, b);
    case BinaryOp::Pow:
      return ValueRange::power(a, b);
    default:
      return ValueRange::range(-kInf, kInf);
    }
  };
  bool isDividing = isInteger && (op == BinaryOp::Div || op == BinaryOp::Rem ||
                                  (op == BinaryOp::Pow && r.lo < 0));
  bool isDivisorOk = !isDividing || this->check.overflowWarnings().checkDivisor(
                                        site, op, op == BinaryOp::Pow ? lhs : rhs, l, r);
  Num exact = apply(l, r);
  if (isDividing) {
    bool isMinByMinusOne = !type->nonOptional()->isUnsigned() && r.lo <= -1 && r.hi >= -1 &&
                           l.lo <= ValueRange::limitsOf(type).lo;
    this->check.overflowWarnings().noteProof(
        site, isDivisorOk && !isMinByMinusOne && !exact.empty() && ValueRange::fits(exact, type));
  }
  if (exact.empty() || !type->nonOptional()->isInteger() || op == BinaryOp::Rem) {
    return this->check.overflowWarnings().fit(exact, type);
  }
  Limits lim = ValueRange::limitsOf(type);
  if (ValueRange::fits(exact, type)) {
    return exact;
  }
  if (!wraps && !this->check.quiet) {
    auto suspect = [&](const Num &n) { return isNarrow ? n : this->blamed(n); };
    Num exceeding = apply(suspect(l), suspect(r));
    bool isConstant = (!lhs || this->check.programScan().isConstantTree(lhs)) &&
                      (!rhs || this->check.programScan().isConstantTree(rhs)) && (lhs || rhs);
    if (!exceeding.empty() && !ValueRange::fits(exceeding, type) && !isConstant) {
      this->check.overflowWarnings().warn(site, l, r, lhs, rhs, exceeding, accum, type);
      return ValueRange::range(lim.lo, lim.hi, Origin::Trusted);
    }
  }
  if (this->check.quiet) {
    return exact;
  }

  return ValueRange::range(lim.lo, lim.hi, wraps ? Origin::Proven : Origin::Trusted);
}

Val Evaluator::evalCast(const CastExpr *conv, State &st, bool wraps) {
  Val v = eval(conv->operand, st, wraps);
  const Type *from = conv->operand->type, *to = conv->type;
  if (!to || !from) {
    return Val();
  }
  if (to->isString()) {
    return ValueRange::lengthVal(ValueRange::someLength());
  }
  if (from->nonOptional()->isJson() && ValueRange::isNumberType(to)) {
    return ValueRange::numVal(this->check.overflowWarnings().input(to, conv->loc(), "JSON"));
  }
  if (to->nonOptional()->isJson() && ValueRange::isIntegerType(from) && !v.num.empty()) {
    this->check.overflowWarnings().checkConversion(conv, conv->operand, v.num, to);
    return Val();
  }
  if (!ValueRange::isNumberType(to)) {
    return ValueRange::hasLength(to) || (from->isResult() && to->isResult()) ? v : Val();
  }
  if (!ValueRange::isNumberType(from)) {
    return this->check.overflowWarnings().trusted(to);
  }
  Num n = v.num;
  if (n.empty()) {
    return Val();
  }
  if (ValueRange::isIntegerType(to) && !ValueRange::isIntegerType(from)) {
    Limits l = ValueRange::limitsOf(to);
    if (n.lo >= l.lo && n.hi <= l.hi) {
      return ValueRange::numVal(ValueRange::make({std::trunc(n.lo), n.loOrigin, n.loWhy},
                                                 {std::trunc(n.hi), n.hiOrigin, n.hiWhy}));
    }
    Bound w = ValueRange::joint(0, n.low(), n.high());
    return ValueRange::numVal(ValueRange::range(l.lo, l.hi, w.origin, w.why));
  }

  return ValueRange::numVal(this->check.overflowWarnings().fit(n, to));
}

}
