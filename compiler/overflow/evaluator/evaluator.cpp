#include "compiler/overflow/evaluator/evaluator.h"
#include "compiler/overflow/program_scan/program_scan.h"
#include "compiler/overflow/slots/slots.h"
#include "compiler/overflow/call_ranges/call_ranges.h"
#include "compiler/overflow/executor/executor.h"
#include "compiler/overflow/loop_ranges/loop_ranges.h"
#include "compiler/overflow/narrowing/narrowing.h"
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

// The times of Date: 100 million days either side of 1970, in milliseconds.
constexpr double kMaxTime = 8.64e15;

} // namespace

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
        element = join(element, type && type->isArray()
                                    ? check_.slots().elementsOf(e.expr, v, type)
                                    : check_.overflowWarnings().trusted(lit->type && lit->type->isArray() ? lit->type->inner()
                                                                                : nullptr));
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
    return evalObjectLit(cast<ObjectLitExpr>(expr), st);
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
    return evalAssign(cast<AssignExpr>(expr), st);
  case Expr::Kind::Update:
    return evalUpdate(cast<UpdateExpr>(expr), st);
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
    // Other tasks run while this one waits, as code a call runs: what the
    // Promise gives is a value of its type.
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

  return check_.overflowWarnings().fitVal(check_.slots().readVar(name->var, st), name->type ? name->type : name->var->type);
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
    // `-2147483648` is a literal.
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
  case UnaryOp::NonNull:
    return eval(unary->operand, st, wraps);
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
  // Bits mix: any value of the type, as worrying as the worse side.
  Num full = mixed(lim.lo, lim.hi, l, r);
  bool lPos = l.lo >= 0, rPos = r.lo >= 0;
  auto zero = Bound{0};
  switch (op) {
  case BinaryOp::BitAnd:
    // A mask that is not negative keeps the value within it.
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
    if (r.lo == r.hi && r.lo >= 0 && r.lo < 31 && lPos) {
      double k = std::ldexp(1.0, int(r.lo));
      if (l.hi * k <= lim.hi) {
        return make({l.lo * k, l.loOrigin, l.loWhy}, {l.hi * k, l.hiOrigin, l.hiWhy});
      }
    }
    return full;
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    unsigned mask = lim.hi > kIntMax ? 63 : 31;
    if (op == BinaryOp::UShr && !lPos) {
      if (r.lo == r.hi && r.lo >= 1 && r.lo < 32) {
        return make(zero,
                    {std::floor(4294967295.0 / std::ldexp(1.0, int(r.lo))), l.loOrigin, l.loWhy});
      }
      return make(zero, {std::min(lim.hi, 4294967295.0), l.loOrigin, l.loWhy});
    }
    if (r.lo == r.hi && r.lo >= 0 && r.lo < 64) {
      double k = std::ldexp(1.0, int(r.lo) & mask);
      return make({std::floor(l.lo / k), l.loOrigin, l.loWhy},
                  {std::floor(l.hi / k), l.hiOrigin, l.hiWhy});
    }
    // Shifted right by some count: between the value and 0.
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
  // The low bits of a sum, difference, product or power depend only on the
  // low bits of the operands; not so for a quotient.
  bool operandsWrap = bits || (wraps && bin->op != BinaryOp::Div && bin->op != BinaryOp::Rem);
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
  if (bits) {
    return numVal(bitwise(bin->op, l.num, r.num, type));
  }
  const VarSymbol *accum = check_.selfAssigned_.lookup(bin);
  if (auto *name = dyn_cast<NameExpr>(stripCasts(bin->lhs));
      !accum && name && check_.accumulators_.contains(name->var)) {
    accum = name->var;
  }

  return numVal(arithmetic(bin, bin->op, l.num, r.num, type, wraps, bin->lhs, bin->rhs, accum));
}

Num Evaluator::arithmetic(const Expr *site, BinaryOp op, const Num &l, const Num &r,
                              const Type *type, bool wraps, const Expr *lhs, const Expr *rhs,
                              const VarSymbol *accum) {
  if (l.empty() || r.empty()) {
    return Num();
  }
  bool integer = isIntegerType(type);
  auto apply = [&](const Num &a, const Num &b) {
    switch (op) {
    case BinaryOp::Add:
      return add(a, b);
    case BinaryOp::Sub:
      return subtract(a, b);
    case BinaryOp::Mul:
      return multiply(a, b);
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
  Num exact = apply(l, r);
  if (exact.empty() || !type->nonOptional()->isInt()) {
    return check_.overflowWarnings().fit(exact, type);
  }
  if (exact.lo >= kIntMin && exact.hi <= kIntMax) {
    return exact;
  }
  if (!wraps && !check_.quiet_) {
    // Trusted ends count as small: the warning is about the others.
    auto blamed = [](Num n) {
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
    };
    Num exceeding = apply(blamed(l), blamed(r));
    bool constant = (!lhs || check_.programScan().isConstantTree(lhs)) && (!rhs || check_.programScan().isConstantTree(rhs)) && (lhs || rhs);
    if ((exceeding.lo < kIntMin || exceeding.hi > kIntMax) && !constant) {
      check_.overflowWarnings().warn(site, l, r, lhs, rhs, exceeding, accum);
      // Reported once: what comes of it is not reported again.
      return range(kIntMin, kIntMax, Origin::Trusted);
    }
  }
  // Before the last pass of a loop: as it is, so that a sum stays a sum.
  if (check_.quiet_) {
    return exact;
  }

  // It wraps around, as `| 0` asks for, or the trusted values stay small.
  return range(kIntMin, kIntMax, wraps ? Origin::Proven : Origin::Trusted);
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
    return hasLength(to) ? v : Val();
  }
  if (!isNumberType(from)) {
    return check_.overflowWarnings().trusted(to);
  }
  Num n = v.num;
  if (n.empty()) {
    return Val();
  }
  if (isIntegerType(to) && !isIntegerType(from)) {
    // A double becomes an int as `x | 0` makes one: the fraction goes, and
    // a value beyond the int range wraps around.
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

Val Evaluator::evalObjectLit(const ObjectLitExpr *lit, State &st) {
  const Type *type = lit->type ? lit->type->nonOptional() : nullptr;
  for (const ObjectLitExpr::Property &p : lit->props) {
    if (p.key) {
      eval(p.key, st);
    }
    Val v = eval(p.value, st);
    if (!type || !type->isObject() || p.method) {
      continue;
    }
    if (p.spread) {
      // The fields it copies keep what they hold.
      const Type *from = p.value->type ? p.value->type->nonOptional() : nullptr;
      if (!from || !from->isObject()) {
        continue;
      }
      for (const Field &f : from->fields()) {
        if (type->field(f.name) && tracked(f.type) &&
            !llvm::is_contained(p.replacedFields, f.name)) {
          check_.slots().storeField(type, f.name, f.type, check_.slots().readField(from, f.name, f.type));
        }
      }
      continue;
    }
    if (const Field *f = p.name.empty() ? nullptr : type->field(p.name)) {
      check_.slots().storeField(type, p.name, f->type, v);
    }
  }

  return Val();
}

OverflowCheck::Place Evaluator::place(const Expr *target, State &st) {
  OverflowCheck::Place p;
  p.type = target->type;
  if (auto *name = dyn_cast<NameExpr>(target)) {
    if (name->var) {
      p.kind = OverflowCheck::Place::Kind::Var;
      p.var = name->var;
      p.type = name->var->type;
    }
    return p;
  }
  if (auto *member = dyn_cast<MemberExpr>(target)) {
    eval(member->object, st);
    const Type *objectType = member->object->type ? member->object->type->nonOptional() : nullptr;
    if (objectType && member->name == "length" && objectType->isArray()) {
      p.kind = OverflowCheck::Place::Kind::Length;
      if (auto *name = dyn_cast<NameExpr>(stripCasts(member->object))) {
        p.var = name->var;
      }
      return p;
    }
    if (objectType && objectType->isObject()) {
      p.kind = OverflowCheck::Place::Kind::Field;
      p.objectType = objectType;
      p.field = member->name.str();
    }
    return p;
  }
  if (auto *index = dyn_cast<IndexExpr>(target)) {
    eval(index->object, st);
    eval(index->index, st);
    const Type *objectType = index->object->type ? index->object->type->nonOptional() : nullptr;
    if (objectType && objectType->isArray()) {
      p.kind = OverflowCheck::Place::Kind::Element;
      p.objectType = objectType;
      p.append = index->mayAppend;
      if (auto *name = dyn_cast<NameExpr>(stripCasts(index->object))) {
        p.var = name->var;
      }
    } else if (objectType && objectType->isObject() && !index->field.empty()) {
      p.kind = OverflowCheck::Place::Kind::Field;
      p.objectType = objectType;
      p.field = index->field.str();
    }
    return p;
  }

  return p;
}

Val Evaluator::read(const OverflowCheck::Place &p, State &st, const Expr *target) {
  switch (p.kind) {
  case OverflowCheck::Place::Kind::Var:
    return check_.slots().readVar(p.var, st);
  case OverflowCheck::Place::Kind::Field:
    return check_.slots().readField(p.objectType, p.field, p.type);
  case OverflowCheck::Place::Kind::Element: {
    const Expr *array = cast<IndexExpr>(target)->object;
    Val arr = p.var ? check_.slots().readVar(p.var, st) : Val();
    if (!p.var) {
      return check_.overflowWarnings().trusted(p.type);
    }
    return check_.slots().elementsOf(array, arr, p.objectType);
  }
  case OverflowCheck::Place::Kind::Length: {
    if (!p.var) {
      return numVal(someLength());
    }
    return numVal(check_.slots().readVar(p.var, st).len);
  }
  case OverflowCheck::Place::Kind::None:
    return check_.overflowWarnings().trusted(p.type);
  }

  return Val();
}

void Evaluator::write(const OverflowCheck::Place &p, const Val &v, State &st) {
  switch (p.kind) {
  case OverflowCheck::Place::Kind::Var:
    check_.slots().writeVar(p.var, v, st);
    return;
  case OverflowCheck::Place::Kind::Field:
    check_.slots().storeField(p.objectType, p.field, p.type, v);
    return;
  case OverflowCheck::Place::Kind::Element: {
    const Type *element = p.objectType->inner();
    check_.contribute(check_.next_.elements[element], check_.overflowWarnings().fitVal(v, element));
    if (p.var) {
      auto found = st.vars.find(p.var);
      if (found != st.vars.end()) {
        Val &array = found->second;
        if (array.elem) {
          array.elem = share(join(*array.elem, check_.overflowWarnings().fitVal(v, element)));
        }
        if (p.append && !array.len.empty()) {
          array.len = narrowTo(join(array.len, add(array.len, exactly(1))), 0, kLengthMax);
        }
      }
    }
    return;
  }
  case OverflowCheck::Place::Kind::Length:
    if (p.var) {
      auto found = st.vars.find(p.var);
      if (found != st.vars.end() && !v.num.empty()) {
        found->second.len = narrowTo(v.num, 0, kLengthMax);
      }
    }
    return;
  case OverflowCheck::Place::Kind::None:
    return;
  }
}

Val Evaluator::evalAssign(const AssignExpr *assign, State &st) {
  if (!assign->op) {
    OverflowCheck::Place p = place(assign->target, st);
    Val v = eval(assign->value, st);
    write(p, v, st);
    return v;
  }
  BinaryOp op = *assign->op;
  OverflowCheck::Place p = place(assign->target, st);
  Val old = read(p, st, assign->target);
  if (op == BinaryOp::LogAnd || op == BinaryOp::LogOr || op == BinaryOp::Nullish) {
    State other = st;
    Val v = eval(assign->value, other);
    st = join(st, other);
    Val result = join(old, v);
    write(p, result, st);
    return result;
  }
  bool bits = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor ||
              op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
  Val v = eval(assign->value, st, bits);
  const Type *type = assign->target->type;
  Val result;
  if (type && type->nonOptional()->isString()) {
    result =
        lengthVal(old.len.empty() || v.len.empty() ? someLength()
                                                   : narrowTo(add(old.len, v.len), 0, kLengthMax));
  } else if (isNumberType(type)) {
    if (bits) {
      result = numVal(bitwise(op, old.num, v.num, type));
    } else {
      const VarSymbol *accum = p.kind == OverflowCheck::Place::Kind::Var ? p.var : nullptr;
      result = numVal(arithmetic(assign, op, old.num, v.num, type, false, assign->target,
                                 assign->value, accum));
    }
  }
  write(p, result, st);

  return result;
}

Val Evaluator::evalUpdate(const UpdateExpr *update, State &st) {
  OverflowCheck::Place p = place(update->target, st);
  Val old = read(p, st, update->target);
  const Type *type = update->target->type;
  if (!isNumberType(type)) {
    return Val();
  }
  Num result = arithmetic(update, update->increment ? BinaryOp::Add : BinaryOp::Sub, old.num,
                          exactly(1), type, false, update->target, nullptr,
                          p.kind == OverflowCheck::Place::Kind::Var ? p.var : nullptr);
  write(p, numVal(result), st);

  return update->prefix ? numVal(result) : old;
}

Val Evaluator::jsonVal(const Type *type, const Reason *why) {
  if (!type) {
    return Val();
  }
  const Type *base = type->nonOptional();
  Val v;
  if (isNumberType(base)) {
    Limits l = limitsOf(base);
    v.num = range(l.lo, l.hi, Origin::Input, why);
  } else if (base->isString()) {
    v.len = someLength();
  } else if (base->isArray()) {
    v.len = someLength();
    v.elem = share(jsonVal(base->inner(), why));
  }

  return v;
}

// What JSON.parse<T> builds puts values from outside into the fields of T and
// of the types inside it.
void Evaluator::jsonValues(const Type *type, const Reason *why,
                               llvm::SmallPtrSetImpl<const Type *> &seen) {
  if (!type) {
    return;
  }
  type = type->nonOptional();
  if (type->isArray()) {
    jsonValues(type->inner(), why, seen);
    return;
  }
  if (type->isUnion()) {
    for (const Type *m : type->members()) {
      jsonValues(m, why, seen);
    }
    return;
  }
  if (type->isDict()) {
    check_.contribute(check_.next_.dicts[type], jsonVal(type->valueType(), why));
    jsonValues(type->valueType(), why, seen);
    return;
  }
  if (!type->isObject() || !seen.insert(type).second) {
    return;
  }
  for (const Field &f : type->fields()) {
    if (tracked(f.type)) {
      check_.slots().storeField(type, f.name, f.type, jsonVal(f.type, why));
    }
    jsonValues(f.type, why, seen);
  }
}

Val Evaluator::evalMath(const CallExpr *call, State &st) {
  std::vector<Num> args;
  for (const Expr *arg : call->args) {
    Val v = eval(arg, st);
    // Math.max(...a): the elements of a.
    const Type *type = arg->type ? arg->type->nonOptional() : nullptr;
    if (type && type->isArray()) {
      v = check_.slots().elementsOf(arg, v, type);
    }
    args.push_back(v.num);
  }
  const Type *type = call->type;
  auto arg = [&](size_t i) { return i < args.size() ? args[i] : Num(); };
  Num x = arg(0);
  auto monotone = [&](double (*f)(double), double lo, double hi) {
    if (x.empty()) {
      return Num();
    }
    double a = f(std::clamp(x.lo, lo, hi)), b = f(std::clamp(x.hi, lo, hi));
    if (std::isnan(a) || std::isnan(b)) {
      return range(-kInf, kInf);
    }

    return make({a, x.loOrigin, x.loWhy}, {b, x.hiOrigin, x.hiWhy});
  };
  Num r;
  switch (call->builtin) {
  case Builtin::MathFloor:
  case Builtin::MathCeil:
  case Builtin::MathRound:
  case Builtin::MathTrunc: {
    if (x.empty()) {
      return Val();
    }
    r = make({std::floor(x.lo), x.loOrigin, x.loWhy}, {std::ceil(x.hi), x.hiOrigin, x.hiWhy});
    // An integer result must fit, or the program stops (see emitMathCall).
    Limits l = limitsOf(type);
    if (isIntegerType(type)) {
      r = narrowTo(r, l.lo, l.hi);
    }
    return numVal(r);
  }
  case Builtin::MathAbs:
    if (x.empty()) {
      return Val();
    }
    r = make({x.lo >= 0   ? x.lo
              : x.hi <= 0 ? -x.hi
                          : 0},
             std::fabs(x.lo) > std::fabs(x.hi) ? Bound{-x.lo, x.loOrigin, x.loWhy} : x.high());
    // Math.abs of the smallest int is itself.
    if (isIntegerType(type) && r.hi > limitsOf(type).hi) {
      r = range(limitsOf(type).lo, limitsOf(type).hi, x.loOrigin, x.loWhy);
    }
    return numVal(r);
  case Builtin::MathSign:
    return numVal(range(-1, 1));
  case Builtin::MathMin:
  case Builtin::MathMax: {
    bool isMin = call->builtin == Builtin::MathMin;
    if (args.empty()) {
      return numVal(exactly(isMin ? kInf : -kInf));
    }
    Num best = args[0];
    for (size_t i = 1; i < args.size(); ++i) {
      const Num &a = args[i];
      if (a.empty() || best.empty()) {
        best = Num();
        break;
      }
      // Each end comes from the side that gives it.
      best = isMin ? make(lower(best.low(), a.low()), lower(best.high(), a.high()))
                   : make(higher(best.low(), a.low()), higher(best.high(), a.high()));
    }
    return numVal(check_.overflowWarnings().fit(best, type));
  }
  case Builtin::MathSqrt:
    return numVal(monotone([](double v) { return std::sqrt(v); }, 0, kInf));
  case Builtin::MathCbrt:
    return numVal(monotone([](double v) { return std::cbrt(v); }, -kInf, kInf));
  case Builtin::MathLog:
  case Builtin::MathLog2:
  case Builtin::MathLog10:
  case Builtin::MathLog1p:
    return numVal(
        make({-kInf}, {x.empty() || x.hi == kInf ? kInf : std::log2(std::max(x.hi, 1.0)) + 1,
                       x.hiOrigin, x.hiWhy}));
  case Builtin::MathSin:
  case Builtin::MathCos:
  case Builtin::MathTanh:
    return numVal(range(-1, 1));
  case Builtin::MathAtan:
  case Builtin::MathAtan2:
  case Builtin::MathAsin:
  case Builtin::MathAcos:
    return numVal(range(-3.15, 3.15));
  case Builtin::MathRandom:
    return numVal(range(0, 0.9999999999999999));
  case Builtin::MathClz32:
    return numVal(range(0, 32));
  case Builtin::MathImul:
    return numVal(range(kIntMin, kIntMax));
  case Builtin::MathFround:
    return numVal(x);
  default:
    return check_.overflowWarnings().trusted(type);
  }
}

Val Evaluator::evalBuiltin(const CallExpr *call, State &st) {
  if (callbackArg(call->builtin)) {
    return check_.callRanges().evalCallback(call, st);
  }
  if (isMathBuiltin(call->builtin)) {
    return evalMath(call, st);
  }
  auto *member = dyn_cast<MemberExpr>(call->callee);
  const Expr *receiverExpr = member ? member->object : nullptr;
  Val receiver = receiverExpr ? eval(receiverExpr, st) : Val();
  const Type *receiverType =
      receiverExpr && receiverExpr->type ? receiverExpr->type->nonOptional() : nullptr;
  std::vector<Val> args;
  for (const Expr *arg : call->args) {
    args.push_back(eval(arg, st));
  }
  const Type *type = call->type;
  auto arg = [&](size_t i) { return i < args.size() ? args[i] : Val(); };
  auto length = [&]() { return receiver.len.empty() ? someLength() : receiver.len; };
  // The length of the receiver, after the call changed it.
  auto setLength = [&](Num len) {
    auto *name = dyn_cast_or_null<NameExpr>(stripCasts(receiverExpr));
    if (!name || !name->var) {
      return;
    }
    auto found = st.vars.find(name->var);
    if (found != st.vars.end() && !len.empty()) {
      found->second.len = narrowTo(len, 0, kLengthMax);
    }
  };
  switch (call->builtin) {
  case Builtin::ParseInt:
    return numVal(check_.overflowWarnings().input(type, call->loc(), "parseInt"));
  case Builtin::ParseFloat:
    return numVal(check_.overflowWarnings().input(type, call->loc(), "parseFloat"));
  case Builtin::NumberOf: {
    const Type *from =
        call->args.empty() || !call->args[0]->type ? nullptr : call->args[0]->type->nonOptional();
    if (from && from->isString()) {
      return numVal(check_.overflowWarnings().input(type, call->loc(), "Number()"));
    }
    if (from && isNumberType(from)) {
      return numVal(arg(0).num);
    }
    if (from && from->isBool()) {
      return numVal(range(0, 1));
    }
    return check_.overflowWarnings().trusted(type);
  }
  case Builtin::JsonParseAs:
  case Builtin::FromJson: {
    const Reason *why = check_.overflowWarnings().reason(Origin::Input, call->loc(), "JSON");
    llvm::SmallPtrSet<const Type *, 8> seen;
    jsonValues(type, why, seen);
    return jsonVal(type, why);
  }
  case Builtin::StrCharCodeAt:
    return numVal(range(0, 65535));
  case Builtin::StrCodePointAt:
    return numVal(range(0, 0x10FFFF));
  case Builtin::StrIndexOf:
  case Builtin::StrLastIndexOf:
  case Builtin::StrSearch:
  case Builtin::ArrIndexOf:
  case Builtin::ArrLastIndexOf: {
    // A position, or -1.
    Num len = length();
    return numVal(make({-1}, {std::max(len.hi - 1, -1.0), len.hiOrigin, len.hiWhy}));
  }
  case Builtin::ArrPush:
  case Builtin::ArrUnshift: {
    Val values;
    double count = 0;
    bool spreads = false;
    for (size_t i = 0; i < args.size(); ++i) {
      if (call->spreads(i)) {
        spreads = true;
        values = join(values, check_.slots().elementsOf(call->args[i], args[i], call->args[i]->type));
      } else {
        values = join(values, args[i]);
        ++count;
      }
    }
    check_.slots().storeElements(receiverExpr, receiverType, values, st);
    Num len = spreads ? make({length().lo + count}, {kLengthMax, Origin::Trusted})
                      : add(length(), exactly(count));
    setLength(len);
    return numVal(narrowTo(len, 0, kLengthMax));
  }
  case Builtin::ArrPop:
  case Builtin::ArrShift: {
    Num len = length();
    setLength(narrowTo(add(len, exactly(-1)), 0, kLengthMax));
    return check_.overflowWarnings().fitVal(check_.slots().elementsOf(receiverExpr, receiver, receiverType), type);
  }
  case Builtin::ArrAt:
    return check_.overflowWarnings().fitVal(check_.slots().elementsOf(receiverExpr, receiver, receiverType), type);
  case Builtin::ArrSlice: {
    Val v;
    v.len = make({0}, length().high());
    v.elem = share(check_.slots().elementsOf(receiverExpr, receiver, receiverType));
    return v;
  }
  case Builtin::ArrConcat: {
    Val v;
    Val element = check_.slots().elementsOf(receiverExpr, receiver, receiverType);
    Num len = length();
    for (size_t i = 0; i < args.size(); ++i) {
      const Type *t = call->args[i]->type ? call->args[i]->type->nonOptional() : nullptr;
      if (t && t->isArray()) {
        element = join(element, check_.slots().elementsOf(call->args[i], args[i], t));
        len = add(len, args[i].len.empty() ? someLength() : args[i].len);
      } else {
        element = join(element, args[i]);
        len = add(len, exactly(1));
      }
    }
    v.len = narrowTo(len, 0, kLengthMax);
    v.elem = share(element);
    return v;
  }
  case Builtin::ArrFill:
    check_.slots().storeElements(receiverExpr, receiverType, arg(0), st);
    return receiver;
  case Builtin::ArrSplice: {
    Val inserted;
    for (size_t i = 2; i < args.size(); ++i) {
      inserted = join(inserted, args[i]);
    }
    if (!isBottom(inserted)) {
      check_.slots().storeElements(receiverExpr, receiverType, inserted, st);
    }
    Num len = length();
    double removed =
        args.size() > 1 && !args[1].num.empty() ? std::max(args[1].num.hi, 0.0) : len.hi;
    double added = double(args.size() > 2 ? args.size() - 2 : 0);
    setLength(make({std::max(len.lo - removed, 0.0)}, {len.hi + added, len.hiOrigin, len.hiWhy}));
    Val v;
    v.len = make({0}, len.high());
    v.elem = share(check_.slots().elementsOf(receiverExpr, receiver, receiverType));
    return v;
  }
  case Builtin::ArrReverse:
  case Builtin::ArrSort:
    return receiver;
  case Builtin::StrSplit:
  case Builtin::StrSplitRegex: {
    Val v;
    v.len = someLength();
    v.elem = share(lengthVal(make({0}, length().high())));
    return v;
  }
  case Builtin::StrAt:
  case Builtin::StrCharAt:
    return lengthVal(range(0, 2));
  case Builtin::MapGet: {
    const Type *value =
        receiverType && receiverType->isDict() ? receiverType->valueType() : nullptr;
    if (std::optional<Val> v = check_.summary(check_.cur_.dicts, receiverType)) {
      return check_.overflowWarnings().fitVal(*v, value);
    }
    return check_.overflowWarnings().trusted(value);
  }
  case Builtin::MapSet:
  case Builtin::RecordSet:
  case Builtin::RecordNullishSet: {
    if (receiverType && receiverType->isDict() && args.size() > 1) {
      check_.contribute(check_.next_.dicts[receiverType], check_.overflowWarnings().fitVal(args[1], receiverType->valueType()));
    }
    if (call->builtin == Builtin::RecordNullishSet) {
      const Type *value =
          receiverType && receiverType->isDict() ? receiverType->valueType() : nullptr;
      std::optional<Val> v = check_.summary(check_.cur_.dicts, receiverType);
      return join(v ? check_.overflowWarnings().fitVal(*v, value) : check_.overflowWarnings().trusted(value), arg(1));
    }
    if (call->builtin == Builtin::RecordSet) {
      return arg(1);
    }
    return receiver;
  }
  case Builtin::MapSize:
    return numVal(someLength());
  case Builtin::DictNew:
  case Builtin::DictGroupBy:
    if (type && type->isDict() && isNumberType(type->valueType()) && !args.empty()) {
      check_.contribute(check_.next_.dicts[type], check_.overflowWarnings().trusted(type->valueType()));
    }
    return Val();
  case Builtin::DateNow:
  case Builtin::PerformanceNow:
    return numVal(range(0, kMaxTime));
  case Builtin::DateGetTime:
  case Builtin::DateUTC:
  case Builtin::DateParse:
    return numVal(range(-kMaxTime, kMaxTime));
  case Builtin::DateGetUTC:
  case Builtin::DateGetLocal:
    return numVal(range(-300000, 300000));
  case Builtin::ProcessArgv:
  case Builtin::ObjectKeys:
  case Builtin::ObjectValues:
  case Builtin::ObjectEntries: {
    Val v;
    v.len = someLength();
    const Type *element = type && type->isArray() ? type->inner() : nullptr;
    v.elem = share(check_.overflowWarnings().trusted(element));
    return v;
  }
  default:
    break;
  }
  // Methods of strings that give strings, and the rest.
  if (call->builtin >= Builtin::RegExpNew && call->builtin <= Builtin::StrSplitRegex) {
    check_.callRanges().calledUserCode(st); // a replacer function runs
  }
  if (isFsBuiltin(call->builtin) || isChildBuiltin(call->builtin)) {
    check_.mayThrow(st);
  }

  return check_.overflowWarnings().trusted(type);
}

} // namespace tz
