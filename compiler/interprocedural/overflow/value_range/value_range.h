#pragma once

#include "compiler/program/ast/ast.h"
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

constexpr double kLengthMax = double(Type::kMaxLength);

constexpr double kDoubleMax = std::numeric_limits<double>::max();

constexpr double kFloatMax = double(std::numeric_limits<float>::max());

enum class Origin : uint8_t {
  Proven,
  Trusted,
  Loop,
  Input,
};

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

struct Limits {
  double lo, hi;
};

struct Val;

using ValPtr = std::shared_ptr<const Val>;

struct Val {
  Num num;
  Num len;
  ValPtr elem;
};

struct State {
  bool dead = false;
  llvm::DenseMap<const VarSymbol *, Val> vars;
};

class ValueRange {
public:
  static Num someLength();
  static State deadState();
  static Val numVal(Num num);
  static ValPtr share(Val v);
  static double ones(double x);
  static Val lengthVal(Num len);
  static Num exactly(double value);
  static Num squared(const Num &a);
  static double awayOf(const Num &n);
  static bool isBottom(const Val &v);
  static bool dangerous(Origin origin);
  static bool tracked(const Type *type);
  static bool provenFinite(const Num &n);
  static double times(double a, double b);
  static bool hasLength(const Type *type);
  static Limits limitsOf(const Type *type);
  static bool isFloatType(const Type *type);
  static Num add(const Num &a, const Num &b);
  static bool isNumberType(const Type *type);
  static Num join(const Num &a, const Num &b);
  static Num meet(const Num &a, const Num &b);
  static bool isIntegerType(const Type *type);
  static bool isDecimalType(const Type *type);
  static Val join(const Val &a, const Val &b);
  static Num power(const Num &a, const Num &b);
  static bool sameNum(const Num &a, const Num &b);
  static Num subtract(const Num &a, const Num &b);
  static Num multiply(const Num &a, const Num &b);
  static bool sameVal(const Val &a, const Val &b);
  static Num narrowTo(Num a, double lo, double hi);
  static bool fits(const Num &n, const Type *type);
  static bool fitsLow(double lo, const Type *type);
  static Num make(const Bound &lo, const Bound &hi);
  static Num powerFloat(const Num &a, const Num &b);
  static bool fitsHigh(double hi, const Type *type);
  static State join(const State &a, const State &b);
  static Bound lower(const Bound &a, const Bound &b);
  static Num widenedDecimal(Num n, double slack = 0);
  static Bound higher(const Bound &a, const Bound &b);
  static double plus(double a, double b, double ifUndefined);
  static Num divide(const Num &a, const Num &b, bool integer);
  static Bound joint(double v, const Bound &a, const Bound &b);
  static Num narrowTo(Num a, const Bound &lo, const Bound &hi);
  static Num mixed(double lo, double hi, const Num &a, const Num &b);
  static Num remainder(const Num &a, const Num &b, bool integer = true);
  static Num range(double lo, double hi, Origin origin = Origin::Proven,
                   const Reason *why = nullptr);
};

}
