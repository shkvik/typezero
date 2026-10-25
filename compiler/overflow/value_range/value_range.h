#pragma once

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

namespace tz {

// The values the analysis computes: a range of a number with where its bounds come
// from, a length, and the state of the variables at a point.

constexpr double kInf = std::numeric_limits<double>::infinity();

constexpr double kIntMin = -2147483648.0;

constexpr double kIntMax = 2147483647.0;

constexpr double kLongMin = -9223372036854775808.0;

constexpr double kLongMax = 9223372036854775807.0;

constexpr double kLengthMax = double(kMaxLength);

// Where the bounds of a value come from, from the least to the most worrying.
enum class Origin : uint8_t {
  // Literals, constants, checks and what is computed from them: the value
  // can really be anywhere in its range.
  Proven,
  // The program's own data that the pass does not follow: fields and
  // elements a program updates, results of recursion, parameters of
  // functions any code may call, lengths it does not know. Its range is
  // wide, but a warning about it would mostly be noise.
  Trusted,
  // Grows with every pass of a loop, which may run any number of times.
  Loop,
  // From outside the program: parseInt, Number of a string, JSON.
  Input,
};

bool dangerous(Origin origin);

// Why a value may be large, for the message.
struct Reason {
  Origin origin = Origin::Input;
  llvm::SMLoc loc;
  std::string what;       // Input: "parseInt", "JSON"...
  bool unbounded = false; // Loop: the loop has no bound on its passes
  // A parameter: the calls that pass it a dangerous value, and the reason of
  // the first one.
  std::vector<llvm::SMLoc> calls;
  const Reason *via = nullptr;
};

// One end of a range, and where it comes from.
struct Bound {
  double v;
  Origin origin = Origin::Proven;
  const Reason *why = nullptr;
};

// The bound computed from `a` and `b`: as worrying as the worse of them.
Bound joint(double v, const Bound &a, const Bound &b);

// A range of numbers, empty when no value gets there, with the origin of
// each end: in `Math.min(parseInt(s), 100)` the top is proven, the bottom
// comes from input.
struct Num {
  double lo = kInf, hi = -kInf;
  Origin loOrigin = Origin::Proven, hiOrigin = Origin::Proven;
  const Reason *loWhy = nullptr, *hiWhy = nullptr;
  // While a loop is being run: the value is what a variable held at the head
  // of the pass plus [offLo, offHi]; 0 for no such relation.
  uint64_t base = 0;
  double offLo = 0, offHi = 0;

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

// The length of a string or an array that is not known: bounded, but a sum
// of such lengths is the program's business (it is bounded by memory).
Num someLength();

// The lower and the higher of two bounds; of equal ones, the worse.
Bound lower(const Bound &a, const Bound &b);

Bound higher(const Bound &a, const Bound &b);

Num join(const Num &a, const Num &b);

// `a` known to lie within [lo, hi]: an end that moves takes the origin of the
// bound that moves it. The relation to a loop's head stays: the value is the
// same.
Num narrowTo(Num a, const Bound &lo, const Bound &hi);

// A clamp to [lo, hi], which changes the origin only of an end that moves.
Num narrowTo(Num a, double lo, double hi);

Num meet(const Num &a, const Num &b);

bool sameNum(const Num &a, const Num &b);

double plus(double a, double b, double ifUndefined);

double times(double a, double b);

Num add(const Num &a, const Num &b);

Num subtract(const Num &a, const Num &b);

Num multiply(const Num &a, const Num &b);

// The worst origin among the ends of a and b, for results that mix them.
Num mixed(double lo, double hi, const Num &a, const Num &b);

// Integers divide truncating; a divisor of 0 stops the program.
Num divide(const Num &a, const Num &b, bool integer);

// The remainder has the sign of the dividend and is smaller than the divisor:
// `x % 100` is small whatever x is.
Num remainder(const Num &a, const Num &b);

Num power(const Num &a, const Num &b);

// The smallest 2^k - 1 at or above x >= 0.
double ones(double x);

struct Limits {
  double lo, hi;
};

Limits limitsOf(const Type *type);

bool isNumberType(const Type *type);

bool isIntegerType(const Type *type);

// Strings and arrays have lengths; arrays elements.
bool hasLength(const Type *type);

// The values the pass follows: numbers, strings and arrays.
bool tracked(const Type *type);

// A value: a number, or a string or an array with its length, and an array's
// elements (null: not known).
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

// What a variable can hold at some point of the program.
struct State {
  bool dead = false; // the point cannot be reached
  llvm::DenseMap<const VarSymbol *, Val> vars;
};

State join(const State &a, const State &b);

State deadState();

} // namespace tz
