#include "compiler/pipeline/check_program/overflow/value_range/value_range.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/facts/facts.h"
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

bool ValueRange::dangerous(Origin origin) {
  return origin >= Origin::Loop;
}

Bound ValueRange::joint(double v, const Bound &a, const Bound &b) {
  const Bound &w = b.origin > a.origin || (b.origin == a.origin && !a.why) ? b : a;

  return {v, w.origin, w.why};
}

Num ValueRange::range(double lo, double hi, Origin origin, const Reason *why) {
  Num n;
  n.lo = lo;
  n.hi = hi;
  n.setOrigin(origin, why);

  return n;
}

Num ValueRange::make(const Bound &lo, const Bound &hi) {
  Num n;
  n.setLow(lo);
  n.setHigh(hi);

  return n;
}

Num ValueRange::exactly(double value) {
  return ValueRange::range(value, value);
}

Num ValueRange::someLength() {
  return ValueRange::make({0}, {kLengthMax, Origin::Trusted});
}

Bound ValueRange::lower(const Bound &a, const Bound &b) {
  if (a.v != b.v) {
    return a.v < b.v ? a : b;
  }

  return ValueRange::joint(a.v, a, b);
}

Bound ValueRange::higher(const Bound &a, const Bound &b) {
  if (a.v != b.v) {
    return a.v > b.v ? a : b;
  }

  return ValueRange::joint(a.v, a, b);
}

Num ValueRange::join(const Num &a, const Num &b) {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  Num r =
      ValueRange::make(ValueRange::lower(a.low(), b.low()), ValueRange::higher(a.high(), b.high()));
  r.away = std::min(ValueRange::awayOf(a), ValueRange::awayOf(b));
  if (a.base && a.base == b.base) {
    r.base = a.base;
    r.offLo = std::min(a.offLo, b.offLo);
    r.offHi = std::max(a.offHi, b.offHi);
  }

  return r;
}

Num ValueRange::narrowTo(Num a, const Bound &lo, const Bound &hi) {
  if (lo.v > a.lo || (lo.v == a.lo && lo.origin < a.loOrigin)) {
    a.setLow(lo);
  }
  if (hi.v < a.hi || (hi.v == a.hi && hi.origin < a.hiOrigin)) {
    a.setHigh(hi);
  }

  return a;
}

Num ValueRange::narrowTo(Num a, double lo, double hi) {
  if (lo > a.lo) {
    a.setLow({lo});
  }
  if (hi < a.hi) {
    a.setHigh({hi});
  }

  return a;
}

Num ValueRange::meet(const Num &a, const Num &b) {
  if (a.empty() || b.empty()) {
    return Num();
  }
  Num r = ValueRange::narrowTo(b, a.low(), a.high());
  r.away = std::max(a.away, b.away);

  return r;
}

bool ValueRange::sameNum(const Num &a, const Num &b) {
  if (a.empty() || b.empty()) {
    return a.empty() == b.empty();
  }

  return a.lo == b.lo && a.hi == b.hi && a.loOrigin == b.loOrigin && a.hiOrigin == b.hiOrigin &&
         a.away == b.away;
}

double ValueRange::plus(double a, double b, double ifUndefined) {
  double r = a + b;

  return std::isnan(r) ? ifUndefined : r;
}

double ValueRange::times(double a, double b) {
  return a == 0 || b == 0 ? 0 : a * b;
}

Num ValueRange::add(const Num &a, const Num &b) {
  if (a.empty() || b.empty()) {
    return Num();
  }
  Num r =
      ValueRange::make(ValueRange::joint(ValueRange::plus(a.lo, b.lo, -kInf), a.low(), b.low()),
                       ValueRange::joint(ValueRange::plus(a.hi, b.hi, kInf), a.high(), b.high()));
  if (a.base) {
    r.base = a.base;
    r.offLo = ValueRange::plus(a.offLo, b.lo, -kInf);
    r.offHi = ValueRange::plus(a.offHi, b.hi, kInf);
  } else if (b.base) {
    r.base = b.base;
    r.offLo = ValueRange::plus(b.offLo, a.lo, -kInf);
    r.offHi = ValueRange::plus(b.offHi, a.hi, kInf);
  }

  return r;
}

Num ValueRange::subtract(const Num &a, const Num &b) {
  if (a.empty() || b.empty()) {
    return Num();
  }
  Num r =
      ValueRange::make(ValueRange::joint(ValueRange::plus(a.lo, -b.hi, -kInf), a.low(), b.high()),
                       ValueRange::joint(ValueRange::plus(a.hi, -b.lo, kInf), a.high(), b.low()));
  if (a.base) {
    r.base = a.base;
    r.offLo = ValueRange::plus(a.offLo, -b.hi, -kInf);
    r.offHi = ValueRange::plus(a.offHi, -b.lo, kInf);
  }

  return r;
}

Num ValueRange::multiply(const Num &a, const Num &b) {
  if (a.empty() || b.empty()) {
    return Num();
  }
  Bound corners[] = {ValueRange::joint(ValueRange::times(a.lo, b.lo), a.low(), b.low()),
                     ValueRange::joint(ValueRange::times(a.lo, b.hi), a.low(), b.high()),
                     ValueRange::joint(ValueRange::times(a.hi, b.lo), a.high(), b.low()),
                     ValueRange::joint(ValueRange::times(a.hi, b.hi), a.high(), b.high())};
  Bound lo = corners[0], hi = corners[0];
  for (const Bound &c : corners) {
    lo = ValueRange::lower(lo, c);
    hi = ValueRange::higher(hi, c);
  }

  return ValueRange::make(lo, hi);
}

Num ValueRange::mixed(double lo, double hi, const Num &a, const Num &b) {
  Bound w = ValueRange::joint(0, ValueRange::joint(0, a.low(), a.high()),
                              ValueRange::joint(0, b.low(), b.high()));

  return ValueRange::range(lo, hi, w.origin, w.why);
}

Num ValueRange::divide(const Num &a, const Num &b, bool integer) {
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
  } else if (double gap = ValueRange::awayOf(b); gap > 0) {
    part(b.lo, -gap);
    part(gap, b.hi);
  } else {
    lo = -kInf;
    hi = kInf;
  }
  if (lo > hi) {
    return Num();
  }
  Num r = ValueRange::mixed(lo, hi, a, b);
  if (integer && b.lo >= 1) {
    r.loOrigin = a.loOrigin;
    r.loWhy = a.loWhy;
    r.hiOrigin = a.hiOrigin;
    r.hiWhy = a.hiWhy;
  }

  return r;
}

Num ValueRange::remainder(const Num &a, const Num &b, bool integer) {
  if (a.empty() || b.empty()) {
    return Num();
  }
  Bound m = std::fabs(b.lo) > std::fabs(b.hi) ? Bound{std::fabs(b.lo), b.loOrigin, b.loWhy}
                                              : Bound{std::fabs(b.hi), b.hiOrigin, b.hiWhy};
  double below = m.v == kInf ? kInf : integer ? std::max(m.v - 1, 0.0) : m.v;
  Bound lo = a.lo >= 0 ? Bound{0} : a.lo >= -below ? a.low() : Bound{-below, m.origin, m.why};
  Bound hi = a.hi <= 0 ? Bound{0} : a.hi <= below ? a.high() : Bound{below, m.origin, m.why};

  return ValueRange::make(lo, hi);
}

Num ValueRange::power(const Num &a, const Num &b) {
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

  return ValueRange::mixed(std::min(lo, 1.0), top, a, b);
}

Num ValueRange::powerFloat(const Num &a, const Num &b) {
  if (a.empty() || b.empty()) {
    return Num();
  }
  bool straddles = a.lo <= 0 && a.hi >= 0;
  double small = straddles ? ValueRange::awayOf(a) : std::min(std::fabs(a.lo), std::fabs(a.hi));
  double big = std::max(std::fabs(a.lo), std::fabs(a.hi));
  double lo = kInf, hi = 0;
  for (double m : {small, big}) {
    for (double e : {b.lo, b.hi}) {
      double p = std::pow(m, e);
      if (std::isnan(p)) {
        p = kInf;
      }
      lo = std::min(lo, p);
      hi = std::max(hi, p);
    }
  }
  bool even = b.lo == b.hi && std::fmod(b.lo, 2.0) == 0;
  if (a.lo < 0 && !even) {
    return ValueRange::mixed(-hi, hi, a, b);
  }

  return ValueRange::mixed(lo, hi, a, b);
}

Num ValueRange::squared(const Num &a) {
  if (a.empty()) {
    return Num();
  }
  if (a.lo >= 0) {
    return ValueRange::make({a.lo * a.lo, a.loOrigin, a.loWhy}, {a.hi * a.hi, a.hiOrigin, a.hiWhy});
  }
  if (a.hi <= 0) {
    return ValueRange::make({a.hi * a.hi, a.hiOrigin, a.hiWhy}, {a.lo * a.lo, a.loOrigin, a.loWhy});
  }
  Bound big = std::fabs(a.lo) > std::fabs(a.hi) ? a.low() : a.high();
  double gap = ValueRange::awayOf(a);

  return ValueRange::make({gap * gap}, {big.v * big.v, big.origin, big.why});
}

double ValueRange::awayOf(const Num &n) {
  if (n.empty()) {
    return 0;
  }
  if (n.lo > 0) {
    return n.lo;
  }
  if (n.hi < 0) {
    return -n.hi;
  }

  return n.away;
}

bool ValueRange::provenFinite(const Num &n) {
  return !n.empty() && n.loOrigin != Origin::Trusted && n.hiOrigin != Origin::Trusted;
}

double ValueRange::ones(double x) {
  double r = 1;
  while (r - 1 < x && r < 1e19) {
    r *= 2;
  }

  return r - 1;
}

Limits ValueRange::limitsOf(const Type *type) {
  if (!type) {
    return {-kInf, kInf};
  }
  type = type->nonOptional();
  if (type->isInteger()) {
    return {double(type->intMin()), double(type->intMax())};
  }
  if (type->isFloat()) {
    return {-kDoubleMax, kDoubleMax};
  }
  if (type->isLiteral() && type->literalBase()->isNumeric()) {
    return {type->literalNumber(), type->literalNumber()};
  }
  if (type->isUnion()) {
    Limits r{kInf, -kInf};
    for (const Type *m : type->members()) {
      Limits l = ValueRange::limitsOf(m);
      r = {std::min(r.lo, l.lo), std::max(r.hi, l.hi)};
    }
    return r;
  }

  return {-kInf, kInf};
}

bool ValueRange::fitsLow(double lo, const Type *type) {
  return lo >= ValueRange::limitsOf(type).lo;
}

bool ValueRange::fitsHigh(double hi, const Type *type) {
  Limits l = ValueRange::limitsOf(type);
  type = type->nonOptional();
  bool rounded = type->isInteger() && type->bitWidth() == 64;

  return rounded ? hi < l.hi : hi <= l.hi;
}

bool ValueRange::fits(const Num &n, const Type *type) {
  return ValueRange::fitsLow(n.lo, type) && ValueRange::fitsHigh(n.hi, type);
}

bool ValueRange::isNumberType(const Type *type) {
  if (!type) {
    return false;
  }
  type = type->nonOptional();
  if (type->isNumeric() || type->isDecimal()) {
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

bool ValueRange::isIntegerType(const Type *type) {
  if (!type) {
    return false;
  }
  type = type->nonOptional();

  return type->isInteger() || (type->isLiteral() && type->literalBase()->isInteger());
}

bool ValueRange::isFloatType(const Type *type) {
  if (!type) {
    return false;
  }
  type = type->nonOptional();

  return type->isFloat() || (type->isLiteral() && type->literalBase()->isFloat());
}

bool ValueRange::isDecimalType(const Type *type) {
  return type && type->nonOptional()->isDecimal();
}

Num ValueRange::widenedDecimal(Num n, double slack) {
  if (n.empty()) {
    return n;
  }
  double tiny = std::numeric_limits<double>::denorm_min();
  double lo = std::nextafter(n.lo - slack, -kInf), hi = std::nextafter(n.hi + slack, kInf);
  if (slack == 0 && n.lo >= 0) {
    lo = std::max(lo, n.lo > 0 ? tiny : 0.0);
  }
  if (slack == 0 && n.hi <= 0) {
    hi = std::min(hi, n.hi < 0 ? -tiny : 0.0);
  }
  n.lo = lo;
  n.hi = hi;

  return n;
}

bool ValueRange::hasLength(const Type *type) {
  if (!type) {
    return false;
  }
  type = type->nonOptional();

  return type->isString() || type->isArray();
}

bool ValueRange::tracked(const Type *type) {
  return ValueRange::isNumberType(type) || ValueRange::hasLength(type) ||
         (type && type->isResult() && ValueRange::tracked(type->okType()));
}

Val ValueRange::numVal(Num num) {
  Val v;
  v.num = num;

  return v;
}

Val ValueRange::lengthVal(Num len) {
  Val v;
  v.len = len;

  return v;
}

bool ValueRange::isBottom(const Val &v) {
  return v.num.empty() && v.len.empty() && !v.elem;
}

ValPtr ValueRange::share(Val v) {
  return std::make_shared<const Val>(std::move(v));
}

Val ValueRange::join(const Val &a, const Val &b) {
  if (ValueRange::isBottom(a)) {
    return b;
  }
  if (ValueRange::isBottom(b)) {
    return a;
  }
  Val r;
  r.num = ValueRange::join(a.num, b.num);
  r.len = ValueRange::join(a.len, b.len);
  if (a.elem && b.elem) {
    r.elem = a.elem == b.elem ? a.elem : ValueRange::share(ValueRange::join(*a.elem, *b.elem));
  }

  return r;
}

bool ValueRange::sameVal(const Val &a, const Val &b) {
  if (!ValueRange::sameNum(a.num, b.num) || !ValueRange::sameNum(a.len, b.len)) {
    return false;
  }
  if (!a.elem || !b.elem) {
    return !a.elem == !b.elem;
  }

  return a.elem == b.elem || ValueRange::sameVal(*a.elem, *b.elem);
}

State ValueRange::join(const State &a, const State &b) {
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
      found->second = ValueRange::join(found->second, entry.second);
    }
  }

  return r;
}

State ValueRange::deadState() {
  State s;
  s.dead = true;

  return s;
}

}
