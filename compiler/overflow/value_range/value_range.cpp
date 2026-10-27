#include "compiler/overflow/value_range/value_range.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/facts/facts.h"
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

bool dangerous(Origin origin) { return origin >= Origin::Loop; }

Bound joint(double v, const Bound &a, const Bound &b) {
  const Bound &w = b.origin > a.origin || (b.origin == a.origin && !a.why) ? b : a;

  return {v, w.origin, w.why};
}

Num range(double lo, double hi, Origin origin, const Reason *why) {
  Num n;
  n.lo = lo;
  n.hi = hi;
  n.setOrigin(origin, why);

  return n;
}

Num make(const Bound &lo, const Bound &hi) {
  Num n;
  n.setLow(lo);
  n.setHigh(hi);

  return n;
}

Num exactly(double value) { return range(value, value); }

Num someLength() { return make({0}, {kLengthMax, Origin::Trusted}); }

Bound lower(const Bound &a, const Bound &b) {
  if (a.v != b.v) {
    return a.v < b.v ? a : b;
  }

  return joint(a.v, a, b);
}

Bound higher(const Bound &a, const Bound &b) {
  if (a.v != b.v) {
    return a.v > b.v ? a : b;
  }

  return joint(a.v, a, b);
}

Num join(const Num &a, const Num &b) {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  Num r = make(lower(a.low(), b.low()), higher(a.high(), b.high()));
  if (a.base && a.base == b.base) {
    r.base = a.base;
    r.offLo = std::min(a.offLo, b.offLo);
    r.offHi = std::max(a.offHi, b.offHi);
  }

  return r;
}

Num narrowTo(Num a, const Bound &lo, const Bound &hi) {
  if (lo.v > a.lo || (lo.v == a.lo && lo.origin < a.loOrigin)) {
    a.setLow(lo);
  }
  if (hi.v < a.hi || (hi.v == a.hi && hi.origin < a.hiOrigin)) {
    a.setHigh(hi);
  }

  return a;
}

Num narrowTo(Num a, double lo, double hi) {
  if (lo > a.lo) {
    a.setLow({lo});
  }
  if (hi < a.hi) {
    a.setHigh({hi});
  }

  return a;
}

Num meet(const Num &a, const Num &b) {
  if (a.empty() || b.empty()) {
    return Num();
  }

  return narrowTo(b, a.low(), a.high());
}

bool sameNum(const Num &a, const Num &b) {
  if (a.empty() || b.empty()) {
    return a.empty() == b.empty();
  }

  return a.lo == b.lo && a.hi == b.hi && a.loOrigin == b.loOrigin && a.hiOrigin == b.hiOrigin;
}

double plus(double a, double b, double ifUndefined) {
  double r = a + b;

  return std::isnan(r) ? ifUndefined : r;
}

double times(double a, double b) { return a == 0 || b == 0 ? 0 : a * b; }

Num add(const Num &a, const Num &b) {
  if (a.empty() || b.empty()) {
    return Num();
  }
  Num r = make(joint(plus(a.lo, b.lo, -kInf), a.low(), b.low()),
               joint(plus(a.hi, b.hi, kInf), a.high(), b.high()));
  if (a.base) {
    r.base = a.base;
    r.offLo = plus(a.offLo, b.lo, -kInf);
    r.offHi = plus(a.offHi, b.hi, kInf);
  } else if (b.base) {
    r.base = b.base;
    r.offLo = plus(b.offLo, a.lo, -kInf);
    r.offHi = plus(b.offHi, a.hi, kInf);
  }

  return r;
}

Num subtract(const Num &a, const Num &b) {
  if (a.empty() || b.empty()) {
    return Num();
  }
  Num r = make(joint(plus(a.lo, -b.hi, -kInf), a.low(), b.high()),
               joint(plus(a.hi, -b.lo, kInf), a.high(), b.low()));
  if (a.base) {
    r.base = a.base;
    r.offLo = plus(a.offLo, -b.hi, -kInf);
    r.offHi = plus(a.offHi, -b.lo, kInf);
  }

  return r;
}

Num multiply(const Num &a, const Num &b) {
  if (a.empty() || b.empty()) {
    return Num();
  }
  Bound corners[] = {
      joint(times(a.lo, b.lo), a.low(), b.low()), joint(times(a.lo, b.hi), a.low(), b.high()),
      joint(times(a.hi, b.lo), a.high(), b.low()), joint(times(a.hi, b.hi), a.high(), b.high())};
  Bound lo = corners[0], hi = corners[0];
  for (const Bound &c : corners) {
    lo = lower(lo, c);
    hi = higher(hi, c);
  }

  return make(lo, hi);
}

Num mixed(double lo, double hi, const Num &a, const Num &b) {
  Bound w = joint(0, joint(0, a.low(), a.high()), joint(0, b.low(), b.high()));

  return range(lo, hi, w.origin, w.why);
}

Num divide(const Num &a, const Num &b, bool integer) {
  if (a.empty() || b.empty()) {
    return Num();
  }
  double lo = kInf, hi = -kInf;
  auto part = [&](double dlo, double dhi) {
    if (dlo > dhi) {
      return;
    }
    for (double x : {a.lo / dlo, a.lo / dhi, a.hi / dlo, a.hi / dhi}) {
      if (std::isnan(x)) {
        x = 0;
      }
      if (integer) {
        x = std::trunc(x);
      }
      lo = std::min(lo, x);
      hi = std::max(hi, x);
    }
  };
  if (integer) {
    part(b.lo, std::min(b.hi, -1.0));
    part(std::max(b.lo, 1.0), b.hi);
  } else if (b.lo > 0 || b.hi < 0) {
    part(b.lo, b.hi);
  } else {
    lo = -kInf;
    hi = kInf;
  }
  if (lo > hi) {
    return Num();
  }
  Num r = mixed(lo, hi, a, b);
  if (integer && b.lo >= 1) {
    r.loOrigin = a.loOrigin;
    r.loWhy = a.loWhy;
    r.hiOrigin = a.hiOrigin;
    r.hiWhy = a.hiWhy;
  }

  return r;
}

Num remainder(const Num &a, const Num &b) {
  if (a.empty() || b.empty()) {
    return Num();
  }
  Bound m = std::fabs(b.lo) > std::fabs(b.hi) ? Bound{std::fabs(b.lo), b.loOrigin, b.loWhy}
                                              : Bound{std::fabs(b.hi), b.hiOrigin, b.hiWhy};
  double below = m.v == kInf ? kInf : std::max(m.v - 1, 0.0);
  Bound lo = a.lo >= 0 ? Bound{0} : a.lo >= -below ? a.low() : Bound{-below, m.origin, m.why};
  Bound hi = a.hi <= 0 ? Bound{0} : a.hi <= below ? a.high() : Bound{below, m.origin, m.why};

  return make(lo, hi);
}

Num power(const Num &a, const Num &b) {
  if (a.empty() || b.empty()) {
    return Num();
  }
  double e = std::max(b.hi, 0.0);
  double m = std::max(std::fabs(a.lo), std::fabs(a.hi));
  double top = std::pow(m, e);
  double lo = a.lo >= 0 ? std::pow(a.lo, std::max(b.lo, 0.0)) : -top;
  if (a.lo >= 0 && a.lo < 1) {
    lo = 0;
  }

  return mixed(std::min(lo, 1.0), top, a, b);
}

double ones(double x) {
  double r = 1;
  while (r - 1 < x && r < 1e19) {
    r *= 2;
  }

  return r - 1;
}

Limits limitsOf(const Type *type) {
  if (!type) {
    return {-kInf, kInf};
  }
  type = type->nonOptional();
  if (type->isInteger()) {
    return {double(type->intMin()), double(type->intMax())};
  }
  if (type->isLiteral() && type->literalBase()->isNumeric()) {
    return {type->literalNumber(), type->literalNumber()};
  }
  if (type->isUnion()) {
    Limits r{kInf, -kInf};
    for (const Type *m : type->members()) {
      Limits l = limitsOf(m);
      r = {std::min(r.lo, l.lo), std::max(r.hi, l.hi)};
    }
    return r;
  }

  return {-kInf, kInf};
}

bool isNumberType(const Type *type) {
  if (!type) {
    return false;
  }
  type = type->nonOptional();
  if (type->isNumeric()) {
    return true;
  }
  if (type->isLiteral()) {
    return type->literalBase()->isNumeric();
  }
  if (type->isUnion()) {
    return llvm::all_of(type->members(), [](const Type *m) {
      return m->isLiteral() && m->literalBase()->isNumeric();
    });
  }

  return false;
}

bool isIntegerType(const Type *type) {
  if (!type) {
    return false;
  }
  type = type->nonOptional();

  return type->isInteger() || (type->isLiteral() && type->literalBase()->isInteger());
}

bool hasLength(const Type *type) {
  if (!type) {
    return false;
  }
  type = type->nonOptional();

  return type->isString() || type->isArray();
}

bool tracked(const Type *type) {
  return isNumberType(type) || hasLength(type) ||
         (type && type->isResult() && tracked(type->okType()));
}

Val numVal(Num num) {
  Val v;
  v.num = num;

  return v;
}

Val lengthVal(Num len) {
  Val v;
  v.len = len;

  return v;
}

bool isBottom(const Val &v) { return v.num.empty() && v.len.empty() && !v.elem; }

ValPtr share(Val v) { return std::make_shared<const Val>(std::move(v)); }

Val join(const Val &a, const Val &b) {
  if (isBottom(a)) {
    return b;
  }
  if (isBottom(b)) {
    return a;
  }
  Val r;
  r.num = join(a.num, b.num);
  r.len = join(a.len, b.len);
  if (a.elem && b.elem) {
    r.elem = a.elem == b.elem ? a.elem : share(join(*a.elem, *b.elem));
  }

  return r;
}

bool sameVal(const Val &a, const Val &b) {
  if (!sameNum(a.num, b.num) || !sameNum(a.len, b.len)) {
    return false;
  }
  if (!a.elem || !b.elem) {
    return !a.elem == !b.elem;
  }

  return a.elem == b.elem || sameVal(*a.elem, *b.elem);
}

State join(const State &a, const State &b) {
  if (a.dead) {
    return b;
  }
  if (b.dead) {
    return a;
  }
  State r = a;
  for (const auto &entry : b.vars) {
    auto found = r.vars.find(entry.first);
    if (found == r.vars.end()) {
      r.vars.insert(entry);
    } else {
      found->second = join(found->second, entry.second);
    }
  }

  return r;
}

State deadState() {
  State s;
  s.dead = true;

  return s;
}

}
