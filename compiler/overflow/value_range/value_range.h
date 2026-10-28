#pragma once

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

namespace tz {

constexpr double kInf = std::numeric_limits<double>::infinity();

constexpr double kInt32Min = -2147483648.0;

constexpr double kInt32Max = 2147483647.0;

constexpr double kIntMin = -9223372036854775808.0;

constexpr double kIntMax = 9223372036854775808.0;

constexpr double kLengthMax = double(kMaxLength);

constexpr double kDoubleMax = std::numeric_limits<double>::max();

constexpr double kFloatMax = double(std::numeric_limits<float>::max());

enum class Origin : uint8_t {
  Proven,
  Trusted,
  Loop,
  Input,
};

bool dangerous(Origin origin);

struct Reason {
  Origin origin = Origin::Input;
  llvm::SMLoc loc;
  std::string what;
  bool unbounded = false;
  std::vector<llvm::SMLoc> calls;
  const Reason *via = nullptr;
};

struct Bound {
  double v;
  Origin origin = Origin::Proven;
  const Reason *why = nullptr;
};

Bound joint(double v, const Bound &a, const Bound &b);

struct Num {
  double lo = kInf, hi = -kInf;
  Origin loOrigin = Origin::Proven, hiOrigin = Origin::Proven;
  const Reason *loWhy = nullptr, *hiWhy = nullptr;
  uint64_t base = 0;
  double offLo = 0, offHi = 0;
  double away = 0;

  bool empty() const { return !(lo <= hi); }
  Bound low() const { return {lo, loOrigin, loWhy}; }
  Bound high() const { return {hi, hiOrigin, hiWhy}; }
  void setLow(const Bound &b) {
    lo = b.v;
    loOrigin = b.origin;
    loWhy = b.why;
  }
  void setHigh(const Bound &b) {
    hi = b.v;
    hiOrigin = b.origin;
    hiWhy = b.why;
  }
  void setOrigin(Origin origin, const Reason *why) {
    loOrigin = hiOrigin = origin;
    loWhy = hiWhy = why;
  }
  Origin worst() const { return std::max(loOrigin, hiOrigin); }
};

Num range(double lo, double hi, Origin origin = Origin::Proven, const Reason *why = nullptr);

Num make(const Bound &lo, const Bound &hi);

Num exactly(double value);

Num someLength();

Bound lower(const Bound &a, const Bound &b);

Bound higher(const Bound &a, const Bound &b);

Num join(const Num &a, const Num &b);

Num narrowTo(Num a, const Bound &lo, const Bound &hi);

Num narrowTo(Num a, double lo, double hi);

Num meet(const Num &a, const Num &b);

bool sameNum(const Num &a, const Num &b);

double plus(double a, double b, double ifUndefined);

double times(double a, double b);

Num add(const Num &a, const Num &b);

Num subtract(const Num &a, const Num &b);

Num multiply(const Num &a, const Num &b);

Num mixed(double lo, double hi, const Num &a, const Num &b);

Num divide(const Num &a, const Num &b, bool integer);

Num remainder(const Num &a, const Num &b, bool integer = true);

Num power(const Num &a, const Num &b);

Num powerFloat(const Num &a, const Num &b);

Num squared(const Num &a);

double awayOf(const Num &n);

bool provenFinite(const Num &n);

double ones(double x);

struct Limits {
  double lo, hi;
};

Limits limitsOf(const Type *type);

bool fits(const Num &n, const Type *type);

bool fitsLow(double lo, const Type *type);

bool fitsHigh(double hi, const Type *type);

bool isNumberType(const Type *type);

bool isIntegerType(const Type *type);

bool isFloatType(const Type *type);

bool isDecimalType(const Type *type);

Num widenedDecimal(Num n, double slack = 0);

bool hasLength(const Type *type);

bool tracked(const Type *type);

struct Val;

using ValPtr = std::shared_ptr<const Val>;

struct Val {
  Num num;
  Num len;
  ValPtr elem;
};

Val numVal(Num num);

Val lengthVal(Num len);

bool isBottom(const Val &v);

ValPtr share(Val v);

Val join(const Val &a, const Val &b);

bool sameVal(const Val &a, const Val &b);

struct State {
  bool dead = false;
  llvm::DenseMap<const VarSymbol *, Val> vars;
};

State join(const State &a, const State &b);

State deadState();

}
