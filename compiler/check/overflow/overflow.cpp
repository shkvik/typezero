// Warnings where int arithmetic may overflow (docs/small.md, section 6).
//
// The pass runs after Sema, over the checked program, and runs every function
// abstractly: a number is a range [lo, hi] together with where its bounds come
// from, a string or an array has a range for its length, and an array a value
// for its elements. Conditions narrow the ranges (`if (x < 100)`), loops are
// run until their ranges settle, and values go from function to function
// through summaries of the whole program: the parameters of a function take
// what all its calls pass, a call gives what the function returns, and fields,
// elements of arrays, values of dictionaries and variables that several
// functions use take everything stored into them anywhere. The summaries are
// computed in rounds until they settle; the last round reports.
//
// Where the bounds of a value come from decides whether it is worth a warning
// (see Origin). A value from outside the program (parseInt, JSON) or one that
// grows with every pass of a loop without a bound may well be large; so may a
// value the pass computes exactly, such as `i * i` with i up to an array's
// length. But the program's own data that the pass does not follow closely (a
// field a loop increments, a result of recursion) is trusted: a warning about
// every `node.count + 1` would be noise.

#include "compiler/check/overflow/overflow.h"

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

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kIntMin = -2147483648.0;
constexpr double kIntMax = 2147483647.0;
constexpr double kLongMin = -9223372036854775808.0;
constexpr double kLongMax = 9223372036854775807.0;
constexpr double kLengthMax = double(kMaxLength);
// The times of Date: 100 million days either side of 1970, in milliseconds.
constexpr double kMaxTime = 8.64e15;
// A summary that keeps growing after this many rounds goes to the end of its
// type's range; the rounds stop at the limit in any case.
constexpr unsigned kGrowthsBeforeWidening = 3;
constexpr unsigned kMaxRounds = 30;
// A loop that runs at most this many passes is run pass by pass.
constexpr double kUnrollPasses = 24;

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

bool dangerous(Origin origin) { return origin >= Origin::Loop; }

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
Bound joint(double v, const Bound &a, const Bound &b) {
  const Bound &w = b.origin > a.origin || (b.origin == a.origin && !a.why) ? b : a;
  return {v, w.origin, w.why};
}

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

Num range(double lo, double hi, Origin origin = Origin::Proven, const Reason *why = nullptr) {
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

// The length of a string or an array that is not known: bounded, but a sum
// of such lengths is the program's business (it is bounded by memory).
Num someLength() { return make({0}, {kLengthMax, Origin::Trusted}); }

// The lower and the higher of two bounds; of equal ones, the worse.
Bound lower(const Bound &a, const Bound &b) {
  if (a.v != b.v)
    return a.v < b.v ? a : b;
  return joint(a.v, a, b);
}

Bound higher(const Bound &a, const Bound &b) {
  if (a.v != b.v)
    return a.v > b.v ? a : b;
  return joint(a.v, a, b);
}

Num join(const Num &a, const Num &b) {
  if (a.empty())
    return b;
  if (b.empty())
    return a;
  Num r = make(lower(a.low(), b.low()), higher(a.high(), b.high()));
  if (a.base && a.base == b.base) {
    r.base = a.base;
    r.offLo = std::min(a.offLo, b.offLo);
    r.offHi = std::max(a.offHi, b.offHi);
  }
  return r;
}

// `a` known to lie within [lo, hi]: an end that moves takes the origin of the
// bound that moves it. The relation to a loop's head stays: the value is the
// same.
Num narrowTo(Num a, const Bound &lo, const Bound &hi) {
  if (lo.v > a.lo || (lo.v == a.lo && lo.origin < a.loOrigin))
    a.setLow(lo);
  if (hi.v < a.hi || (hi.v == a.hi && hi.origin < a.hiOrigin))
    a.setHigh(hi);
  return a;
}

// A clamp to [lo, hi], which changes the origin only of an end that moves.
Num narrowTo(Num a, double lo, double hi) {
  if (lo > a.lo)
    a.setLow({lo});
  if (hi < a.hi)
    a.setHigh({hi});
  return a;
}

Num meet(const Num &a, const Num &b) {
  if (a.empty() || b.empty())
    return Num();
  return narrowTo(b, a.low(), a.high());
}

bool sameNum(const Num &a, const Num &b) {
  if (a.empty() || b.empty())
    return a.empty() == b.empty();
  return a.lo == b.lo && a.hi == b.hi && a.loOrigin == b.loOrigin && a.hiOrigin == b.hiOrigin;
}

double plus(double a, double b, double ifUndefined) {
  double r = a + b;
  return std::isnan(r) ? ifUndefined : r;
}

double times(double a, double b) { return a == 0 || b == 0 ? 0 : a * b; }

Num add(const Num &a, const Num &b) {
  if (a.empty() || b.empty())
    return Num();
  Num r = make(joint(plus(a.lo, b.lo, -kInf), a.low(), b.low()),
               joint(plus(a.hi, b.hi, kInf), a.high(), b.high()));
  // A sum keeps the relation of its left side, `sum + i` that of sum.
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
  if (a.empty() || b.empty())
    return Num();
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
  if (a.empty() || b.empty())
    return Num();
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

// The worst origin among the ends of a and b, for results that mix them.
Num mixed(double lo, double hi, const Num &a, const Num &b) {
  Bound w = joint(0, joint(0, a.low(), a.high()), joint(0, b.low(), b.high()));
  return range(lo, hi, w.origin, w.why);
}

// Integers divide truncating; a divisor of 0 stops the program.
Num divide(const Num &a, const Num &b, bool integer) {
  if (a.empty() || b.empty())
    return Num();
  double lo = kInf, hi = -kInf;
  auto part = [&](double dlo, double dhi) {
    if (dlo > dhi)
      return;
    for (double x : {a.lo / dlo, a.lo / dhi, a.hi / dlo, a.hi / dhi}) {
      if (std::isnan(x))
        x = 0;
      if (integer)
        x = std::trunc(x);
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
  if (lo > hi)
    return Num();
  Num r = mixed(lo, hi, a, b);
  // By a positive integer, the quotient is no larger than the dividend.
  if (integer && b.lo >= 1) {
    r.loOrigin = a.loOrigin;
    r.loWhy = a.loWhy;
    r.hiOrigin = a.hiOrigin;
    r.hiWhy = a.hiWhy;
  }
  return r;
}

// The remainder has the sign of the dividend and is smaller than the divisor:
// `x % 100` is small whatever x is.
Num remainder(const Num &a, const Num &b) {
  if (a.empty() || b.empty())
    return Num();
  Bound m = std::fabs(b.lo) > std::fabs(b.hi) ? Bound{std::fabs(b.lo), b.loOrigin, b.loWhy}
                                              : Bound{std::fabs(b.hi), b.hiOrigin, b.hiWhy};
  double below = m.v == kInf ? kInf : std::max(m.v - 1, 0.0);
  Bound lo = a.lo >= 0 ? Bound{0} : a.lo >= -below ? a.low() : Bound{-below, m.origin, m.why};
  Bound hi = a.hi <= 0 ? Bound{0} : a.hi <= below ? a.high() : Bound{below, m.origin, m.why};
  return make(lo, hi);
}

Num power(const Num &a, const Num &b) {
  if (a.empty() || b.empty())
    return Num();
  double e = std::max(b.hi, 0.0);
  double m = std::max(std::fabs(a.lo), std::fabs(a.hi));
  double top = std::pow(m, e);
  double lo = a.lo >= 0 ? std::pow(a.lo, std::max(b.lo, 0.0)) : -top;
  if (a.lo >= 0 && a.lo < 1)
    lo = 0;
  return mixed(std::min(lo, 1.0), top, a, b);
}

// The smallest 2^k - 1 at or above x >= 0.
double ones(double x) {
  double r = 1;
  while (r - 1 < x && r < 1e19)
    r *= 2;
  return r - 1;
}

struct Limits {
  double lo, hi;
};

Limits limitsOf(const Type *type) {
  if (!type)
    return {-kInf, kInf};
  type = type->nonOptional();
  if (type->isInt())
    return {kIntMin, kIntMax};
  if (type->isInteger())
    return {kLongMin, kLongMax};
  if (type->isLiteral() && type->literalBase()->isNumeric())
    return {type->literalNumber(), type->literalNumber()};
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
  if (!type)
    return false;
  type = type->nonOptional();
  if (type->isNumeric())
    return true;
  if (type->isLiteral())
    return type->literalBase()->isNumeric();
  if (type->isUnion())
    return llvm::all_of(type->members(), [](const Type *m) {
      return m->isLiteral() && m->literalBase()->isNumeric();
    });
  return false;
}

bool isIntegerType(const Type *type) {
  if (!type)
    return false;
  type = type->nonOptional();
  return type->isInteger() || (type->isLiteral() && type->literalBase()->isInteger());
}

// Strings and arrays have lengths; arrays elements.
bool hasLength(const Type *type) {
  if (!type)
    return false;
  type = type->nonOptional();
  return type->isString() || type->isArray();
}

// The values the pass follows: numbers, strings and arrays.
bool tracked(const Type *type) { return isNumberType(type) || hasLength(type); }

// A value: a number, or a string or an array with its length, and an array's
// elements (null: not known).
struct Val;
using ValPtr = std::shared_ptr<const Val>;
struct Val {
  Num num;
  Num len;
  ValPtr elem;
};

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
  if (isBottom(a))
    return b;
  if (isBottom(b))
    return a;
  Val r;
  r.num = join(a.num, b.num);
  r.len = join(a.len, b.len);
  if (a.elem && b.elem)
    r.elem = a.elem == b.elem ? a.elem : share(join(*a.elem, *b.elem));
  return r;
}

bool sameVal(const Val &a, const Val &b) {
  if (!sameNum(a.num, b.num) || !sameNum(a.len, b.len))
    return false;
  if (!a.elem || !b.elem)
    return !a.elem == !b.elem;
  return a.elem == b.elem || sameVal(*a.elem, *b.elem);
}

// What a variable can hold at some point of the program.
struct State {
  bool dead = false; // the point cannot be reached
  llvm::DenseMap<const VarSymbol *, Val> vars;
};

State join(const State &a, const State &b) {
  if (a.dead)
    return b;
  if (b.dead)
    return a;
  State r = a;
  for (const auto &entry : b.vars) {
    auto found = r.vars.find(entry.first);
    if (found == r.vars.end())
      r.vars.insert(entry);
    else
      found->second = join(found->second, entry.second);
  }
  return r;
}

State deadState() {
  State s;
  s.dead = true;
  return s;
}

const Expr *stripCasts(const Expr *expr) {
  while (auto *conv = dyn_cast_or_null<CastExpr>(expr)) {
    if (!conv->isImplicit())
      break;
    expr = conv->operand;
  }
  return expr;
}

// The functions of arrays and maps whose callback runs during the call, and
// the position of the callback among the arguments.
std::optional<size_t> callbackArg(Builtin builtin) {
  switch (builtin) {
  case Builtin::ArrForEach:
  case Builtin::ArrMap:
  case Builtin::ArrFilter:
  case Builtin::ArrFlatMap:
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight:
  case Builtin::ArrFind:
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLast:
  case Builtin::ArrFindLastIndex:
  case Builtin::ArrSome:
  case Builtin::ArrEvery:
  case Builtin::ArrSortWith:
  case Builtin::MapForEach:
    return 0;
  case Builtin::ArrayFrom:
    return 1;
  default:
    return std::nullopt;
  }
}

// The builtins that read their array arguments and keep nothing of them.
bool onlyReads(Builtin builtin) {
  switch (builtin) {
  case Builtin::ConsoleLog:
  case Builtin::ConsoleError:
  case Builtin::StringOf:
  case Builtin::JsonStringify:
  case Builtin::ArrConcat:
  case Builtin::ArrayIsArray:
  case Builtin::MathMin:
  case Builtin::MathMax:
  case Builtin::ArrayFrom:
    return true;
  default:
    return false;
  }
}

class OverflowCheck {
public:
  OverflowCheck(SourceFile &program, Diagnostics &diags) : program_(program), diags_(diags) {}

  void run();

private:
  //===--------------------------------------------------------------------===//
  // What the program is made of (found once, before the rounds)
  //===--------------------------------------------------------------------===//

  struct VarInfo {
    // The function, run on its own, that declares the variable; null for
    // main's code.
    const FunctionDecl *owner = nullptr;
    bool declared = false;
    // Another function uses it, or assigns to it: then its owner reads it
    // from the summary too.
    bool shared = false;
    bool writtenElsewhere = false;
    // An array that may have other names: what is stored through those
    // reaches it too.
    bool aliased = false;
    const VarDeclarator *decl = nullptr;
    bool isConst = false;
  };

  void scanProgram();
  void addFunction(const FunctionDecl *fn);
  void scanFunction(const FunctionDecl *fn);
  void scanClass(const ClassDecl *cls);
  void declareVar(const VarSymbol *var, const VarDeclarator *decl = nullptr, bool isConst = false);
  void scanStmt(const Stmt *stmt);
  // `consumed`: the value is only read here, not kept under another name.
  void scanExpr(const Expr *expr, bool consumed);
  void useVar(const VarSymbol *var, bool consumed, bool write);
  bool isConstantTree(const Expr *expr) const;

  SourceFile &program_;
  Diagnostics &diags_;
  // Whether the pre-pass is declaring (first walk) or using (second walk).
  bool declaring_ = true;
  const FunctionDecl *scanOwner_ = nullptr;
  std::vector<const FunctionDecl *> functions_;
  llvm::DenseSet<const FunctionDecl *> functionSet_;
  llvm::DenseSet<const FunctionDecl *> inlined_;
  // Functions used as values: any code may call them, with anything.
  llvm::DenseSet<const FunctionDecl *> unknownCallers_;
  llvm::DenseMap<const VarSymbol *, VarInfo> vars_;
  // Constants whose initializers are constants: Sema computes and checks
  // the operations of those.
  llvm::DenseSet<const VarSymbol *> constants_;
  // For hints: the declarator an expression initializes, and the variable
  // that `x = x + ...` assigns its sum to.
  llvm::DenseMap<const Expr *, std::pair<const VarDeclarator *, bool>> inits_;
  llvm::DenseMap<const Expr *, const VarSymbol *> selfAssigned_;
  llvm::DenseSet<const VarSymbol *> accumulators_;

  //===--------------------------------------------------------------------===//
  // Summaries of the whole program
  //===--------------------------------------------------------------------===//

  struct Slot {
    Val val;
    unsigned growths = 0;
    const Type *type = nullptr; // of a field
  };
  struct Pred {
    std::vector<Num> whenTrue, whenFalse;
  };
  struct Summaries {
    llvm::DenseMap<const FunctionDecl *, std::vector<Slot>> params;
    llvm::DenseMap<const FunctionDecl *, Slot> rets;
    llvm::DenseMap<const VarSymbol *, Slot> vars;
    std::map<std::pair<const Type *, std::string>, Slot> fields;
    llvm::DenseMap<const Type *, Slot> elements; // by element type
    llvm::DenseMap<const Type *, Slot> dicts;    // the values, by dictionary type
    llvm::DenseMap<const FunctionDecl *, Pred> preds;
    // The calls that pass a parameter a dangerous value, and its reason.
    std::map<std::pair<const FunctionDecl *, size_t>,
             std::vector<std::pair<llvm::SMLoc, const Reason *>>>
        dangerousCalls;
  };

  // Adds `v` to the summary `slot` of the round being run.
  void contribute(Slot &slot, const Val &v) {
    if (!quiet_)
      slot.val = join(slot.val, v);
  }
  // The value of a summary: not known in the first round, which only finds
  // which summaries get values; nothing yet for one that gets them later.
  template <typename Map, typename Key>
  std::optional<Val> summary(const Map &map, const Key &key) const {
    if (round_ == 0)
      return std::nullopt;
    auto found = map.find(key);
    if (found == map.end())
      return std::nullopt;
    return found->second.val;
  }
  bool mergeSummaries(bool narrowing);
  bool mergeSlot(Slot &into, const Slot *from, const Type *type, bool narrowing);
  const Reason *paramReason(const FunctionDecl *fn, size_t index);

  Summaries cur_, next_;
  unsigned round_ = 0;
  bool reporting_ = false;

  //===--------------------------------------------------------------------===//
  // Running code
  //===--------------------------------------------------------------------===//

  // Where control goes: a frame's returns, a loop's breaks and continues, a
  // try's catch. What a pass of a loop inside that is not the loop's last
  // pass sends there does not count (see quiet_): those passes run with
  // values that only find the loop's head.
  struct Frame {
    const FunctionDecl *fn = nullptr; // null: main
    bool callback = false;            // a callback run where it is passed
    State returned;                   // callback: the states at its returns
    Val result;                       // callback: what they give
    size_t targets = 0;               // its jump targets start there
    unsigned quiet = 0;
  };
  struct Target {
    bool loop = false;
    bool block = false; // a labeled statement that is no loop
    llvm::SmallVector<llvm::StringRef, 1> labels;
    State breaks = deadState(), conts = deadState();
    unsigned quiet = 0;
  };
  struct Try {
    State *thrown;
    unsigned quiet;
  };
  // A state that may go to the catch around, as where something throws.
  void mayThrow(const State &st) {
    if (!tries_.empty() && tries_.back().quiet == quiet_)
      *tries_.back().thrown = join(*tries_.back().thrown, st);
  }
  struct LoopSpec {
    llvm::SMLoc loc;
    llvm::SmallVector<llvm::StringRef, 1> labels;
    const Expr *cond = nullptr;      // tested before each pass
    const Expr *condAfter = nullptr; // tested after each pass (do-while)
    const Expr *step = nullptr;
    bool exitAtHead = false; // for-of and callbacks: may stop before any pass
    std::function<void(State &)> body;
    // A bound on the states at the head: the passes, and the state the loop
    // ends in after the last; empty: none.
    Num passes;
    // A loop that nothing in the program's data has to end: no condition,
    // or one that reads the input. Its counters may grow without a bound.
    bool openEnded = false;
    // No loop inside: a pass costs little, and the loop may run pass by
    // pass.
    bool simple = false;
  };

  void analyzeAll();
  void analyzeFunction(const FunctionDecl *fn);
  Val paramValue(const FunctionDecl *fn, size_t index);

  void exec(const Stmt *stmt, State &st);
  void execLabeled(const LabeledStmt *stmt, State &st);
  void execLoop(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels, State &st);
  void execTry(const TryStmt *stmt, State &st);
  void execSwitch(const SwitchStmt *stmt, State &st);
  void execReturn(const ReturnStmt *ret, State &st);
  void jump(const Stmt *stmt, llvm::StringRef label, bool isBreak, State &st);

  State onePass(const LoopSpec &spec, const State &head, State &exits);
  void runLoop(const LoopSpec &spec, State &st);
  // `from` and `end`: a pass from `head`, each value relative to itself.
  Num passCount(const LoopSpec &spec, const State &entry, const State &head, const State &from,
                const State &end);
  State rebased(const State &state, llvm::DenseMap<const VarSymbol *, uint64_t> &bases);

  Val eval(const Expr *expr, State &st, bool wraps = false);
  Val evalName(const NameExpr *name, State &st);
  Val evalMember(const MemberExpr *member, State &st);
  Val evalIndex(const IndexExpr *index, State &st);
  Val evalUnary(const UnaryExpr *unary, State &st, bool wraps);
  Val evalBinary(const BinaryExpr *bin, State &st, bool wraps);
  Val evalAssign(const AssignExpr *assign, State &st);
  Val evalUpdate(const UpdateExpr *update, State &st);
  Val evalCall(const CallExpr *call, State &st);
  Val evalBuiltin(const CallExpr *call, State &st);
  Val evalMath(const CallExpr *call, State &st);
  Val evalCallback(const CallExpr *call, State &st);
  Val evalNew(const NewExpr *ne, State &st);
  Val evalObjectLit(const ObjectLitExpr *lit, State &st);
  Val evalCast(const CastExpr *conv, State &st, bool wraps);
  Val runCallback(const FunctionDecl *fn, llvm::ArrayRef<Val> args, size_t first, State &st);
  void passArgs(const FunctionDecl *fn, const CallExpr *call, llvm::ArrayRef<Val> args);
  Val returned(const FunctionDecl *fn, const Type *type);
  void calledUserCode(State &st);

  // Where a value is stored: a variable, a field, an element of an array.
  struct Place {
    enum class Kind { None, Var, Field, Element, Length } kind = Kind::None;
    const VarSymbol *var = nullptr; // Var; the array of Element and Length, if a variable
    const Type *objectType = nullptr;
    std::string field;
    const Type *type = nullptr;
    bool append = false;
  };
  Place place(const Expr *target, State &st);
  Val read(const Place &place, State &st, const Expr *target);
  void write(const Place &place, const Val &v, State &st);
  Val readVar(const VarSymbol *var, const State &st);
  void writeVar(const VarSymbol *var, Val v, State &st);
  Val readField(const Type *objectType, llvm::StringRef field, const Type *type);
  void storeField(const Type *objectType, llvm::StringRef field, const Type *type, const Val &v);
  Val elementsOf(const Expr *arrayExpr, const Val &array, const Type *arrayType);
  void storeElements(const Expr *arrayExpr, const Type *arrayType, const Val &v, State &st);
  bool isPrivateArray(const Expr *expr) const;
  void jsonValues(const Type *type, const Reason *why, llvm::SmallPtrSetImpl<const Type *> &seen);
  Val jsonVal(const Type *type, const Reason *why);

  void refine(const Expr *cond, State &st, State &whenTrue, State &whenFalse);
  void narrowComparison(const BinaryExpr *bin, const Val &l, const Val &r, State &whenTrue,
                        State &whenFalse);
  void applyPredicate(const CallExpr *call, State &whenTrue, State &whenFalse);
  void notePredicate(const FunctionDecl *fn, const State &whenTrue, const State &whenFalse);
  // A variable or the length of one that a check can narrow: the value
  // checked is `var * scale + offset`, or with `square` var * var.
  struct Narrowable {
    const VarSymbol *var = nullptr;
    bool length = false;
    bool integer = false;
    double scale = 1, offset = 0;
    bool square = false;
  };
  std::optional<Narrowable> narrowable(const Expr *expr, const State &st) const;
  void constrain(State &s, const Narrowable &target, const Bound &lower, const Bound &upper);

  Val trusted(const Type *type) const;
  Val fitVal(Val v, const Type *type) const;
  Num fit(Num n, const Type *type) const;
  const Reason *reason(Origin origin, llvm::SMLoc loc, std::string what, bool unbounded = false);
  Num input(const Type *type, llvm::SMLoc loc, const char *what);

  // An operation of `type` that may overflow, at `site`.
  Num arithmetic(const Expr *site, BinaryOp op, const Num &l, const Num &r, const Type *type,
                 bool wraps, const Expr *lhs, const Expr *rhs, const VarSymbol *accum);
  Num bitwise(BinaryOp op, const Num &l, const Num &r, const Type *type) const;
  void warn(const Expr *site, const Num &l, const Num &r, const Expr *lhs, const Expr *rhs,
            const Num &exceeding, const VarSymbol *accum);
  std::string line(llvm::SMLoc loc, llvm::SMLoc from) const;
  std::string inputText(const Reason *why, llvm::SMLoc from) const;
  std::string fixText(const Expr *site, const VarSymbol *accum) const;

  // Passes of loops that are not the last: nothing is recorded in them.
  unsigned quiet_ = 0;
  // The passes of loop bodies run in this round: past a budget, loops get
  // fewer of them, so that a program of many nested loops compiles in time.
  uint64_t work_ = 0;
  bool overBudget() const { return work_ > 2000000; }
  const FunctionDecl *owner_ = nullptr;
  std::vector<Frame> frames_;
  std::vector<Target> targets_;
  std::vector<Try> tries_;
  uint64_t bases_ = 0;
  std::deque<Reason> reasons_;
  std::map<std::pair<const char *, std::string>, const Reason *> reasonIndex_;
  struct Warning {
    llvm::SMLoc loc;
    std::string message;
  };
  std::vector<Warning> warnings_;
};

//===----------------------------------------------------------------------===//
// The pre-pass
//===----------------------------------------------------------------------===//

void OverflowCheck::addFunction(const FunctionDecl *fn) {
  if (!fn || !fn->body || program_.unusedFunctions.contains(fn))
    return;
  // A generic function runs as its instances.
  if (!fn->typeParams.empty() && !fn->isInstance)
    return;
  if (!functionSet_.insert(fn).second)
    return;
  functions_.push_back(fn);
  if (fn->overrides || fn->overridden)
    unknownCallers_.insert(fn);
}

void OverflowCheck::declareVar(const VarSymbol *var, const VarDeclarator *decl, bool isConst) {
  if (!var || !declaring_)
    return;
  VarInfo &info = vars_[var];
  if (info.declared)
    return;
  info.declared = true;
  info.owner = scanOwner_;
  info.decl = decl;
  info.isConst = isConst;
}

void OverflowCheck::useVar(const VarSymbol *var, bool consumed, bool write) {
  if (!var || declaring_)
    return;
  VarInfo &info = vars_[var];
  bool elsewhere = !info.declared || info.owner != scanOwner_;
  if (elsewhere) {
    info.shared = true;
    if (write)
      info.writtenElsewhere = true;
  }
  if (var->type && var->type->nonOptional()->isArray() && (!consumed || elsewhere))
    info.aliased = true;
}

void OverflowCheck::scanFunction(const FunctionDecl *fn) {
  const FunctionDecl *outer = scanOwner_;
  scanOwner_ = fn;
  for (const Param &p : fn->params) {
    declareVar(p.sym);
    if (!declaring_ && p.sym && p.sym->type && p.sym->type->nonOptional()->isArray())
      vars_[p.sym].aliased = true; // the caller's array
    if (p.defaultValue)
      scanExpr(p.defaultValue, false);
  }
  if (fn->body)
    scanStmt(fn->body);
  scanOwner_ = outer;
}

void OverflowCheck::scanClass(const ClassDecl *cls) {
  if (cls->builtin)
    return;
  for (const FunctionDecl *fn : cls->methods)
    addFunction(fn);
  for (const FunctionDecl *fn : cls->constructors)
    addFunction(fn);
  addFunction(cls->init);
  for (const ClassField &f : cls->fields) {
    if (f.method)
      addFunction(f.method);
    // A static field is a global of main's code, which sets it.
    if (f.isStatic) {
      declareVar(f.var);
      if (f.init)
        scanExpr(f.init, false);
      if (!declaring_ && f.var)
        useVar(f.var, false, true);
    }
  }
}

void OverflowCheck::scanProgram() {
  for (bool declaring : {true, false}) {
    declaring_ = declaring;
    scanOwner_ = nullptr;
    for (const Stmt *item : program_.items) {
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        addFunction(fn);
        continue;
      }
      if (auto *cls = dyn_cast<ClassDecl>(item)) {
        scanClass(cls);
        continue;
      }
      scanStmt(item);
    }
    for (const FunctionDecl *fn : program_.instances)
      addFunction(fn);
    // The functions found so far, and those found inside them as they are
    // scanned.
    for (size_t i = 0; i < functions_.size(); ++i)
      scanFunction(functions_[i]);
  }
}

void OverflowCheck::scanStmt(const Stmt *stmt) {
  if (!stmt)
    return;
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : cast<BlockStmt>(stmt)->body)
      scanStmt(s);
    break;
  case Stmt::Kind::VarDecl: {
    auto *decl = cast<VarDeclStmt>(stmt);
    for (const VarDeclarator &d : decl->decls) {
      declareVar(d.sym, &d, decl->isConst);
      if (!d.init)
        continue;
      // The hidden variable of a pattern only reads what it takes apart.
      scanExpr(d.init, d.destructured);
      if (declaring_) {
        inits_[d.init] = {&d, decl->isConst};
        if (d.sym && decl->isConst && isConstantTree(d.init))
          constants_.insert(d.sym);
      }
    }
    break;
  }
  case Stmt::Kind::Function: {
    // A function declared in a block runs on its own, when it is called.
    auto *fn = cast<FunctionDecl>(stmt);
    if (declaring_)
      addFunction(fn);
    break;
  }
  case Stmt::Kind::Expr:
    scanExpr(cast<ExprStmt>(stmt)->expr, true);
    break;
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    scanExpr(s->cond, true);
    scanStmt(s->thenStmt);
    scanStmt(s->elseStmt);
    break;
  }
  case Stmt::Kind::While:
    scanExpr(cast<WhileStmt>(stmt)->cond, true);
    scanStmt(cast<WhileStmt>(stmt)->body);
    break;
  case Stmt::Kind::DoWhile:
    scanStmt(cast<DoWhileStmt>(stmt)->body);
    scanExpr(cast<DoWhileStmt>(stmt)->cond, true);
    break;
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    scanStmt(s->init);
    scanExpr(s->cond, true);
    scanExpr(s->step, true);
    scanStmt(s->body);
    break;
  }
  case Stmt::Kind::ForOf: {
    auto *s = cast<ForOfStmt>(stmt);
    declareVar(s->binding.sym, &s->binding, s->isConst);
    scanExpr(s->iterable, true);
    scanStmt(s->body);
    break;
  }
  case Stmt::Kind::Switch: {
    auto *s = cast<SwitchStmt>(stmt);
    scanExpr(s->discriminant, true);
    for (const SwitchCase &c : s->cases) {
      scanExpr(c.test, true);
      for (const Stmt *body : c.body)
        scanStmt(body);
    }
    break;
  }
  case Stmt::Kind::Labeled:
    scanStmt(cast<LabeledStmt>(stmt)->body);
    break;
  case Stmt::Kind::Return:
    scanExpr(cast<ReturnStmt>(stmt)->value, false);
    break;
  case Stmt::Kind::Throw:
    scanExpr(cast<ThrowStmt>(stmt)->value, false);
    break;
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    scanStmt(s->block);
    declareVar(s->param.sym, &s->param);
    scanStmt(s->handler);
    scanStmt(s->finalizer);
    break;
  }
  case Stmt::Kind::Class:
    scanClass(cast<ClassDecl>(stmt));
    break;
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue:
  case Stmt::Kind::Empty:
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
  case Stmt::Kind::Debugger:
    break;
  }
}

void OverflowCheck::scanExpr(const Expr *expr, bool consumed) {
  if (!expr)
    return;
  switch (expr->kind()) {
  case Expr::Kind::IntLit:
  case Expr::Kind::FloatLit:
  case Expr::Kind::BoolLit:
  case Expr::Kind::StringLit:
  case Expr::Kind::Undefined:
  case Expr::Kind::RegexLit:
    break;
  case Expr::Kind::Template:
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs)
      scanExpr(e, true);
    break;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements)
      scanExpr(e.expr, e.spread);
    break;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(expr)->props) {
      scanExpr(p.key, true);
      scanExpr(p.value, p.spread);
    }
    break;
  case Expr::Kind::Name: {
    auto *name = cast<NameExpr>(expr);
    useVar(name->var, consumed, false);
    // A function used as a value: any code may call it.
    if (!declaring_) {
      if (name->func && name->func->decl)
        unknownCallers_.insert(name->func->decl);
      if (name->var && name->var->function)
        unknownCallers_.insert(name->var->function);
      if (name->var && name->var->lambda)
        unknownCallers_.insert(name->var->lambda);
    }
    break;
  }
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(expr);
    scanExpr(member->object, true);
    if (!declaring_ && member->method && member->method->decl)
      unknownCallers_.insert(member->method->decl);
    break;
  }
  case Expr::Kind::Index:
    scanExpr(cast<IndexExpr>(expr)->object, true);
    scanExpr(cast<IndexExpr>(expr)->index, true);
    break;
  case Expr::Kind::Unary:
    scanExpr(cast<UnaryExpr>(expr)->operand, true);
    break;
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    bool compares = bin->op == BinaryOp::Eq || bin->op == BinaryOp::Ne;
    // `a ?? b`, `a || b` give one of their sides.
    bool gives =
        bin->op == BinaryOp::Nullish || bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr;
    scanExpr(bin->lhs, compares || !gives);
    scanExpr(bin->rhs, compares || !gives);
    break;
  }
  case Expr::Kind::Assign: {
    auto *assign = cast<AssignExpr>(expr);
    if (auto *name = dyn_cast<NameExpr>(assign->target)) {
      useVar(name->var, true, true);
      if (declaring_ && !assign->op)
        if (auto *bin = dyn_cast<BinaryExpr>(assign->value))
          if (auto *lhs = dyn_cast<NameExpr>(stripCasts(bin->lhs)); lhs && lhs->var == name->var)
            selfAssigned_[bin] = name->var;
    } else {
      scanExpr(assign->target, true);
    }
    scanExpr(assign->value, false);
    break;
  }
  case Expr::Kind::Update: {
    auto *update = cast<UpdateExpr>(expr);
    if (auto *name = dyn_cast<NameExpr>(update->target))
      useVar(name->var, true, true);
    else
      scanExpr(update->target, true);
    break;
  }
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(expr);
    if (auto *member = dyn_cast<MemberExpr>(call->callee)) {
      scanExpr(member->object, true);
    } else if (auto *name = dyn_cast<NameExpr>(call->callee);
               name && (call->func || call->builtin != Builtin::None)) {
      useVar(name->var, true, false); // a direct call, not a use of the function as a value
    } else {
      scanExpr(call->callee, true);
    }
    std::optional<size_t> callback = callbackArg(call->builtin);
    bool reads = onlyReads(call->builtin);
    for (size_t i = 0; i < call->args.size(); ++i) {
      const Expr *arg = call->args[i];
      if (callback && *callback == i) {
        if (auto *fe = dyn_cast<FunctionExpr>(stripCasts(arg))) {
          // Runs during the call, as part of the code around it.
          if (declaring_)
            inlined_.insert(fe->fn);
          // The sum that reduce carries from element to element.
          if (declaring_ && !fe->fn->params.empty() &&
              (call->builtin == Builtin::ArrReduce || call->builtin == Builtin::ArrReduceRight))
            accumulators_.insert(fe->fn->params[0].sym);
          for (const Param &p : fe->fn->params) {
            declareVar(p.sym);
            if (p.defaultValue)
              scanExpr(p.defaultValue, false);
          }
          scanStmt(fe->fn->body);
          continue;
        }
      }
      scanExpr(arg, reads || call->spreads(i));
    }
    break;
  }
  case Expr::Kind::New: {
    auto *ne = cast<NewExpr>(expr);
    for (const Expr *arg : ne->ctor ? ne->ctor->args : ne->args)
      scanExpr(arg, false);
    break;
  }
  case Expr::Kind::Conditional: {
    auto *c = cast<ConditionalExpr>(expr);
    scanExpr(c->cond, true);
    scanExpr(c->thenExpr, consumed);
    scanExpr(c->elseExpr, consumed);
    break;
  }
  case Expr::Kind::Cast: {
    auto *conv = cast<CastExpr>(expr);
    scanExpr(conv->operand, consumed || (conv->type && conv->type->isString()));
    break;
  }
  case Expr::Kind::Sequence: {
    auto &exprs = cast<SequenceExpr>(expr)->exprs;
    for (size_t i = 0; i < exprs.size(); ++i)
      scanExpr(exprs[i], i + 1 < exprs.size() || consumed);
    break;
  }
  case Expr::Kind::OptionalChain:
    scanExpr(cast<OptionalChainExpr>(expr)->chain, consumed);
    break;
  case Expr::Kind::Function: {
    // A closure that runs on its own, whenever something calls it.
    const FunctionDecl *fn = cast<FunctionExpr>(expr)->fn;
    if (declaring_ && !inlined_.contains(fn))
      addFunction(fn);
    break;
  }
  case Expr::Kind::Yield:
    scanExpr(cast<YieldExpr>(expr)->value, false);
    break;
  case Expr::Kind::Await:
    scanExpr(cast<AwaitExpr>(expr)->value, false);
    break;
  }
}

// Literals and operators on them, and constants of those: Sema computes such
// values when compiling and reports those that do not fit.
bool OverflowCheck::isConstantTree(const Expr *expr) const {
  expr = stripCasts(expr);
  if (!expr)
    return false;
  if (isa<IntLitExpr>(expr) || isa<FloatLitExpr>(expr))
    return true;
  if (auto *unary = dyn_cast<UnaryExpr>(expr))
    return unary->op != UnaryOp::Not && isConstantTree(unary->operand);
  if (auto *bin = dyn_cast<BinaryExpr>(expr))
    return isConstantTree(bin->lhs) && isConstantTree(bin->rhs);
  if (auto *name = dyn_cast<NameExpr>(expr))
    return name->var && constants_.contains(name->var);
  return false;
}

//===----------------------------------------------------------------------===//
// Values
//===----------------------------------------------------------------------===//

const Reason *OverflowCheck::reason(Origin origin, llvm::SMLoc loc, std::string what,
                                    bool unbounded) {
  auto key = std::make_pair(loc.getPointer(),
                            what + (unbounded ? "!" : "") + char('0' + static_cast<int>(origin)));
  auto found = reasonIndex_.find(key);
  if (found != reasonIndex_.end())
    return found->second;
  Reason &r = reasons_.emplace_back();
  r.origin = origin;
  r.loc = loc;
  r.what = std::move(what);
  r.unbounded = unbounded;
  reasonIndex_[key] = &r;
  return &r;
}

Num OverflowCheck::input(const Type *type, llvm::SMLoc loc, const char *what) {
  Limits l = limitsOf(type);
  return range(l.lo, l.hi, Origin::Input, reason(Origin::Input, loc, what));
}

// A value of `type` about which nothing is known, but which is the
// program's own.
Val OverflowCheck::trusted(const Type *type) const {
  Val v;
  if (!type)
    return v;
  if (isNumberType(type)) {
    Limits l = limitsOf(type);
    v.num = range(l.lo, l.hi, Origin::Trusted);
  }
  if (hasLength(type))
    v.len = someLength();
  return v;
}

Num OverflowCheck::fit(Num n, const Type *type) const {
  if (n.empty() || !isNumberType(type))
    return n;
  // Before the last pass of a loop: as it is, so that a sum stays a sum.
  if (quiet_ && isIntegerType(type))
    return n;
  Limits l = limitsOf(type);
  if (n.lo >= l.lo && n.hi <= l.hi)
    return n;
  // An integer that does not fit has wrapped around: it may be any value, as
  // worrying as the end that went beyond.
  if (isIntegerType(type)) {
    Bound w = n.hi > l.hi && n.lo < l.lo ? joint(0, n.low(), n.high())
              : n.hi > l.hi              ? n.high()
                                         : n.low();
    return range(l.lo, l.hi, w.origin, w.why);
  }
  return narrowTo(n, l.lo, l.hi);
}

Val OverflowCheck::fitVal(Val v, const Type *type) const {
  if (!type)
    return v;
  if (isNumberType(type))
    v.num = fit(v.num, type);
  if (hasLength(type) && !v.len.empty())
    v.len = narrowTo(v.len, 0, kLengthMax);
  return v;
}

Val OverflowCheck::readVar(const VarSymbol *var, const State &st) {
  auto info = vars_.find(var);
  bool own = info != vars_.end() && info->second.declared && info->second.owner == owner_ &&
             !info->second.writtenElsewhere;
  Val v;
  bool found = false;
  if (own) {
    auto value = st.vars.find(var);
    if (value != st.vars.end()) {
      v = value->second;
      found = true;
    }
  }
  if (!found) {
    std::optional<Val> s = summary(cur_.vars, var);
    v = s ? fitVal(*s, var->type) : trusted(var->type);
  }
  return v;
}

void OverflowCheck::writeVar(const VarSymbol *var, Val v, State &st) {
  if (!var || !tracked(var->type))
    return;
  v = fitVal(std::move(v), var->type);
  auto info = vars_.find(var);
  if (info != vars_.end() && info->second.shared)
    contribute(next_.vars[var], v);
  if (info != vars_.end() && info->second.declared && info->second.owner == owner_ &&
      !info->second.writtenElsewhere)
    st.vars[var] = std::move(v);
}

Val OverflowCheck::readField(const Type *objectType, llvm::StringRef field, const Type *type) {
  if (!tracked(type))
    return Val();
  if (std::optional<Val> v = summary(cur_.fields, std::make_pair(objectType, field.str())))
    return fitVal(*v, type);
  return trusted(type);
}

void OverflowCheck::storeField(const Type *objectType, llvm::StringRef field, const Type *type,
                               const Val &v) {
  if (quiet_ || !objectType || !tracked(type))
    return;
  Slot &slot = next_.fields[{objectType, field.str()}];
  slot.type = type;
  contribute(slot, fitVal(v, type));
}

// Whether no other name may reach the array that `expr` gives: a variable
// whose array has no other names, or an array made right there (a literal,
// `new Array`, what map or slice make).
bool OverflowCheck::isPrivateArray(const Expr *expr) const {
  expr = stripCasts(expr);
  if (!expr)
    return false;
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    if (!name->var)
      return false;
    auto info = vars_.find(name->var);
    return info != vars_.end() && info->second.declared && !info->second.aliased &&
           !info->second.writtenElsewhere && info->second.owner == owner_;
  }
  if (isa<ArrayLitExpr>(expr) || isa<NewExpr>(expr))
    return true;
  if (auto *call = dyn_cast<CallExpr>(expr)) {
    switch (call->builtin) {
    case Builtin::ArrMap:
    case Builtin::ArrFilter:
    case Builtin::ArrFlatMap:
    case Builtin::ArrSlice:
    case Builtin::ArrConcat:
    case Builtin::ArrayFrom:
    case Builtin::StrSplit:
    case Builtin::StrSplitRegex:
    case Builtin::JsonParseAs:
    case Builtin::FromJson:
      return true;
    default:
      return false;
    }
  }
  return false;
}

// The elements of `array`, the value of `arrayExpr`: for an array that may
// have other names, also what is stored through them.
Val OverflowCheck::elementsOf(const Expr *arrayExpr, const Val &array, const Type *arrayType) {
  const Type *element = arrayType && arrayType->nonOptional()->isArray()
                            ? arrayType->nonOptional()->inner()
                            : nullptr;
  if (!element)
    return Val();
  if (!array.elem)
    return trusted(element);
  Val v = *array.elem;
  if (!isPrivateArray(arrayExpr)) {
    if (std::optional<Val> stored = summary(cur_.elements, element))
      v = join(v, *stored);
    else if (round_ == 0)
      return trusted(element);
  }
  return fitVal(v, element);
}

void OverflowCheck::storeElements(const Expr *arrayExpr, const Type *arrayType, const Val &v,
                                  State &st) {
  if (!arrayType || !arrayType->nonOptional()->isArray())
    return;
  const Type *element = arrayType->nonOptional()->inner();
  contribute(next_.elements[element], v);
  auto *name = dyn_cast_or_null<NameExpr>(stripCasts(arrayExpr));
  if (name && name->var) {
    auto found = st.vars.find(name->var);
    if (found != st.vars.end()) {
      Val &array = found->second;
      array.elem = array.elem ? share(join(*array.elem, v)) : nullptr;
    }
  }
}

//===----------------------------------------------------------------------===//
// Summaries
//===----------------------------------------------------------------------===//

// Builds the reason of a parameter that some calls pass a dangerous value.
const Reason *OverflowCheck::paramReason(const FunctionDecl *fn, size_t index) {
  auto found = next_.dangerousCalls.find({fn, index});
  if (found == next_.dangerousCalls.end() || found->second.empty())
    return nullptr;
  Reason &r = reasons_.emplace_back();
  const auto &calls = found->second;
  r.via = calls.front().second;
  r.origin = r.via ? r.via->origin : Origin::Input;
  r.loc = calls.front().first;
  for (const auto &call : calls)
    if (!llvm::is_contained(r.calls, call.first))
      r.calls.push_back(call.first);
  return &r;
}

Val widenVal(const Val &old, const Val &grown, const Type *type) {
  Val r = grown;
  auto widen = [](const Num &o, Num n, Limits l) {
    if (o.empty() || n.empty())
      return n;
    // An end that grows across rounds comes of recursion, or of data that
    // is updated from itself.
    if (n.lo < o.lo) {
      n.lo = l.lo;
      if (!dangerous(n.loOrigin))
        n.loOrigin = Origin::Trusted, n.loWhy = nullptr;
    }
    if (n.hi > o.hi) {
      n.hi = l.hi;
      if (!dangerous(n.hiOrigin))
        n.hiOrigin = Origin::Trusted, n.hiWhy = nullptr;
    }
    return n;
  };
  r.num = widen(old.num, grown.num, limitsOf(type));
  r.len = widen(old.len, grown.len, {0, kLengthMax});
  if (old.elem && grown.elem) {
    const Type *element =
        type && type->nonOptional()->isArray() ? type->nonOptional()->inner() : nullptr;
    r.elem = share(widenVal(*old.elem, *grown.elem, element));
  }
  return r;
}

bool OverflowCheck::mergeSlot(Slot &into, const Slot *from, const Type *type, bool narrowing) {
  if (narrowing) {
    // What the settled summaries give again, within them.
    if (!from)
      return false;
    Val v = into.val;
    Val n = from->val;
    if (!v.num.empty() && !n.num.empty())
      v.num = meet(v.num, n.num);
    if (!v.len.empty() && !n.len.empty())
      v.len = meet(v.len, n.len);
    bool changed = !sameVal(v, into.val);
    into.val = v;
    return changed;
  }
  if (!from)
    return false;
  Val joined = join(into.val, from->val);
  if (sameVal(joined, into.val))
    return false;
  if (++into.growths > kGrowthsBeforeWidening || round_ >= kMaxRounds)
    joined = widenVal(into.val, joined, type);
  into.val = joined;
  return true;
}

bool OverflowCheck::mergeSummaries(bool narrowing) {
  bool changed = false;
  for (auto &entry : cur_.params) {
    const FunctionDecl *fn = entry.first;
    auto from = next_.params.find(fn);
    for (size_t i = 0; i < entry.second.size(); ++i) {
      const Slot *slot =
          from != next_.params.end() && i < from->second.size() ? &from->second[i] : nullptr;
      const Type *type =
          i < fn->params.size() && fn->params[i].sym ? fn->params[i].sym->type : nullptr;
      changed |= mergeSlot(entry.second[i], slot, type, narrowing);
      Num &num = entry.second[i].val.num;
      if (dangerous(num.worst()))
        if (const Reason *why = paramReason(fn, i)) {
          if (dangerous(num.loOrigin))
            num.loWhy = why;
          if (dangerous(num.hiOrigin))
            num.hiWhy = why;
        }
    }
  }
  for (auto &entry : cur_.rets) {
    auto from = next_.rets.find(entry.first);
    const Type *type = entry.first->sym ? entry.first->sym->ret : nullptr;
    changed |= mergeSlot(entry.second, from != next_.rets.end() ? &from->second : nullptr, type,
                         narrowing);
  }
  for (auto &entry : cur_.vars) {
    auto from = next_.vars.find(entry.first);
    changed |= mergeSlot(entry.second, from != next_.vars.end() ? &from->second : nullptr,
                         entry.first->type, narrowing);
  }
  for (auto &entry : cur_.fields) {
    auto from = next_.fields.find(entry.first);
    const Slot *slot = from != next_.fields.end() ? &from->second : nullptr;
    changed |= mergeSlot(entry.second, slot, slot ? slot->type : entry.second.type, narrowing);
  }
  for (auto &entry : cur_.elements) {
    auto from = next_.elements.find(entry.first);
    changed |= mergeSlot(entry.second, from != next_.elements.end() ? &from->second : nullptr,
                         entry.first, narrowing);
  }
  for (auto &entry : cur_.dicts) {
    auto from = next_.dicts.find(entry.first);
    const Type *value = entry.first->valueType();
    changed |= mergeSlot(entry.second, from != next_.dicts.end() ? &from->second : nullptr, value,
                         narrowing);
  }
  if (!narrowing) {
    for (auto &entry : next_.preds) {
      Pred &into = cur_.preds[entry.first];
      const Pred &from = entry.second;
      auto merge = [&](std::vector<Num> &a, const std::vector<Num> &b) {
        if (a.size() < b.size())
          a.resize(b.size());
        for (size_t i = 0; i < b.size(); ++i) {
          Num joined = join(a[i], b[i]);
          if (!sameNum(joined, a[i])) {
            a[i] = joined;
            changed = true;
          }
        }
      };
      merge(into.whenTrue, from.whenTrue);
      merge(into.whenFalse, from.whenFalse);
    }
  }
  return changed;
}

void OverflowCheck::run() {
  scanProgram();
  // The first round finds which summaries get values; their values start
  // empty after it.
  analyzeAll();
  for (auto &entry : next_.params) {
    std::vector<Slot> &slots = cur_.params[entry.first];
    slots.resize(entry.second.size());
  }
  for (auto &entry : next_.rets)
    cur_.rets[entry.first];
  for (auto &entry : next_.vars)
    cur_.vars[entry.first];
  for (auto &entry : next_.fields)
    cur_.fields[entry.first];
  for (auto &entry : next_.elements)
    cur_.elements[entry.first];
  for (auto &entry : next_.dicts)
    cur_.dicts[entry.first];
  for (round_ = 1;; ++round_) {
    next_ = Summaries();
    analyzeAll();
    if (!mergeSummaries(/*narrowing=*/false))
      break;
    if (round_ > kMaxRounds + 4)
      break;
  }
  // Rounds from the settled summaries bring back the bounds that widening
  // gave up.
  for (int i = 0; i < 2; ++i) {
    ++round_;
    next_ = Summaries();
    analyzeAll();
    if (!mergeSummaries(/*narrowing=*/true))
      break;
  }
  ++round_;
  reporting_ = true;
  next_ = Summaries();
  analyzeAll();

  llvm::SourceMgr &sources = diags_.sourceMgr();
  std::stable_sort(warnings_.begin(), warnings_.end(), [&](const Warning &a, const Warning &b) {
    unsigned fa = sources.FindBufferContainingLoc(a.loc),
             fb = sources.FindBufferContainingLoc(b.loc);
    if (fa != fb)
      return fa < fb;
    return a.loc.getPointer() < b.loc.getPointer();
  });
  // A place is reported once, also when several copies of a generic
  // function have it.
  std::set<const char *> shown;
  for (const Warning &w : warnings_)
    if (shown.insert(w.loc.getPointer()).second)
      diags_.warning(w.loc, w.message);
}

//===----------------------------------------------------------------------===//
// Functions
//===----------------------------------------------------------------------===//

void OverflowCheck::analyzeAll() {
  work_ = 0;
  owner_ = nullptr;
  frames_.assign(1, Frame());
  targets_.clear();
  tries_.clear();
  State st;
  for (const Stmt *item : program_.items) {
    if (isa<FunctionDecl>(item))
      continue;
    exec(item, st);
  }
  for (const FunctionDecl *fn : functions_)
    analyzeFunction(fn);
}

Val OverflowCheck::paramValue(const FunctionDecl *fn, size_t index) {
  const Param &p = fn->params[index];
  const Type *type = p.sym->type;
  if (round_ == 0)
    return trusted(type);
  Val v;
  bool known = false;
  auto found = cur_.params.find(fn);
  if (found != cur_.params.end() && index < found->second.size()) {
    v = found->second[index].val;
    known = true;
  }
  if (!known || unknownCallers_.contains(fn))
    v = join(v, trusted(type));
  return fitVal(v, type);
}

void OverflowCheck::analyzeFunction(const FunctionDecl *fn) {
  owner_ = fn;
  frames_.assign(1, Frame());
  frames_.back().fn = fn;
  targets_.clear();
  tries_.clear();
  State st;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const Param &p = fn->params[i];
    if (!p.sym || !tracked(p.sym->type))
      continue;
    Val v = paramValue(fn, i);
    if (p.defaultValue)
      v = join(v, eval(p.defaultValue, st));
    writeVar(p.sym, v, st);
  }
  exec(fn->body, st);
}

// Adds what a call passes to the summaries of the parameters.
void OverflowCheck::passArgs(const FunctionDecl *fn, const CallExpr *call,
                             llvm::ArrayRef<Val> args) {
  if (quiet_ || !fn)
    return;
  std::vector<Slot> &slots = next_.params[fn];
  if (slots.size() < fn->params.size())
    slots.resize(fn->params.size());
  bool spread = call && call->hasSpread();
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const Param &p = fn->params[i];
    if (!p.sym)
      continue;
    Val v;
    if (p.rest) {
      // The arguments from here on, as one array.
      Val element;
      for (size_t k = i; k < args.size(); ++k)
        element = join(element, call && call->spreads(k) && args[k].elem ? *args[k].elem : args[k]);
      v.len = spread ? someLength() : exactly(double(args.size() > i ? args.size() - i : 0));
      v.elem = share(element);
    } else if (i < args.size() && !spread) {
      v = args[i];
    } else if (spread) {
      v = trusted(p.sym->type);
    } else {
      continue; // left out: the default value, which the function itself gives
    }
    slots[i].val = join(slots[i].val, v);
    if (dangerous(v.num.worst()) && call)
      next_.dangerousCalls[{fn, i}].push_back(
          {call->loc(), dangerous(v.num.hiOrigin) ? v.num.hiWhy : v.num.loWhy});
  }
}

Val OverflowCheck::returned(const FunctionDecl *fn, const Type *type) {
  if (std::optional<Val> v = summary(cur_.rets, fn))
    return fitVal(*v, type);
  return trusted(type);
}

// User code may change the length of any array that has other names.
void OverflowCheck::calledUserCode(State &st) {
  for (auto &entry : st.vars) {
    auto info = vars_.find(entry.first);
    if (info != vars_.end() && info->second.aliased && !entry.second.len.empty())
      entry.second.len = someLength();
  }
  mayThrow(st);
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

void OverflowCheck::exec(const Stmt *stmt, State &st) {
  if (!stmt || st.dead)
    return;
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : cast<BlockStmt>(stmt)->body) {
      if (st.dead)
        break;
      exec(s, st);
    }
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls) {
      if (!d.sym)
        continue;
      Val v = d.init ? eval(d.init, st) : Val();
      writeVar(d.sym, v, st);
    }
    break;
  case Stmt::Kind::Function:
  case Stmt::Kind::Empty:
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
  case Stmt::Kind::Debugger:
    break;
  case Stmt::Kind::Expr:
    eval(cast<ExprStmt>(stmt)->expr, st);
    break;
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    State t, f;
    refine(s->cond, st, t, f);
    exec(s->thenStmt, t);
    exec(s->elseStmt, f);
    st = join(t, f);
    break;
  }
  case Stmt::Kind::While:
  case Stmt::Kind::DoWhile:
  case Stmt::Kind::For:
  case Stmt::Kind::ForOf:
    execLoop(stmt, {}, st);
    break;
  case Stmt::Kind::Switch:
    execSwitch(cast<SwitchStmt>(stmt), st);
    break;
  case Stmt::Kind::Labeled:
    execLabeled(cast<LabeledStmt>(stmt), st);
    break;
  case Stmt::Kind::Return:
    execReturn(cast<ReturnStmt>(stmt), st);
    break;
  case Stmt::Kind::Break:
    jump(stmt, cast<BreakStmt>(stmt)->label, true, st);
    break;
  case Stmt::Kind::Continue:
    jump(stmt, cast<ContinueStmt>(stmt)->label, false, st);
    break;
  case Stmt::Kind::Throw:
    eval(cast<ThrowStmt>(stmt)->value, st);
    mayThrow(st);
    st.dead = true;
    break;
  case Stmt::Kind::Try:
    execTry(cast<TryStmt>(stmt), st);
    break;
  case Stmt::Kind::Class: {
    // Declaring a class sets its static fields.
    auto *cls = cast<ClassDecl>(stmt);
    if (cls->builtin)
      break;
    for (const ClassField &f : cls->fields)
      if (f.isStatic && f.var && f.init)
        writeVar(f.var, eval(f.init, st), st);
    break;
  }
  }
}

void OverflowCheck::execLabeled(const LabeledStmt *stmt, State &st) {
  llvm::SmallVector<llvm::StringRef, 1> labels{stmt->label};
  const Stmt *body = stmt->body;
  while (auto *inner = dyn_cast<LabeledStmt>(body)) {
    labels.push_back(inner->label);
    body = inner->body;
  }
  if (isa<WhileStmt>(body) || isa<DoWhileStmt>(body) || isa<ForStmt>(body) ||
      isa<ForOfStmt>(body)) {
    execLoop(body, labels, st);
    return;
  }
  Target target;
  target.block = true;
  target.labels = labels;
  target.quiet = quiet_;
  targets_.push_back(std::move(target));
  exec(body, st);
  State breaks = targets_.back().breaks;
  targets_.pop_back();
  st = join(st, breaks);
}

void OverflowCheck::jump(const Stmt *, llvm::StringRef label, bool isBreak, State &st) {
  size_t floor = frames_.back().targets;
  for (size_t i = targets_.size(); i > floor; --i) {
    Target &t = targets_[i - 1];
    bool matches =
        label.empty() ? (isBreak ? !t.block : t.loop) : llvm::is_contained(t.labels, label);
    if (!matches)
      continue;
    if (t.quiet != quiet_)
      break;
    if (isBreak)
      t.breaks = join(t.breaks, st);
    else
      t.conts = join(t.conts, st);
    break;
  }
  st.dead = true;
}

void OverflowCheck::execSwitch(const SwitchStmt *stmt, State &st) {
  eval(stmt->discriminant, st);
  for (const SwitchCase &c : stmt->cases)
    if (c.test)
      eval(c.test, st);
  Target target;
  target.quiet = quiet_;
  targets_.push_back(std::move(target));
  State fall = deadState();
  bool hasDefault = false;
  for (const SwitchCase &c : stmt->cases) {
    if (!c.test)
      hasDefault = true;
    State in = join(fall, st);
    for (const Stmt *s : c.body) {
      if (in.dead)
        break;
      exec(s, in);
    }
    fall = in;
  }
  State breaks = targets_.back().breaks;
  targets_.pop_back();
  State out = join(fall, breaks);
  if (!hasDefault && !stmt->exhaustive)
    out = join(out, st);
  st = out;
}

void OverflowCheck::execTry(const TryStmt *stmt, State &st) {
  State entry = st;
  State thrown = deadState();
  tries_.push_back({&thrown, quiet_});
  exec(stmt->block, st);
  tries_.pop_back();
  State out = st;
  if (stmt->handler) {
    // Something in the block may throw at any point.
    State handler = join(join(entry, st), thrown);
    exec(stmt->handler, handler);
    out = join(st, handler);
  }
  if (stmt->finalizer) {
    if (out.dead) {
      State fin = join(entry, thrown);
      exec(stmt->finalizer, fin);
    } else {
      exec(stmt->finalizer, out);
    }
  }
  st = out;
}

void OverflowCheck::execReturn(const ReturnStmt *ret, State &st) {
  // The value may run callbacks, which push frames.
  size_t at = frames_.size() - 1;
  Val v;
  if (ret->value) {
    const FunctionDecl *fn = frames_[at].fn;
    const Type *retType = fn && fn->sym ? fn->sym->ret : nullptr;
    if (!frames_[at].callback && retType && retType->isBool()) {
      // A function that tests: what holds when it returns true, and false.
      State t, f;
      refine(ret->value, st, t, f);
      notePredicate(fn, t, f);
    } else {
      v = eval(ret->value, st);
    }
  }
  Frame &frame = frames_[at];
  if (frame.callback) {
    if (frame.quiet == quiet_) {
      frame.result = join(frame.result, v);
      frame.returned = join(frame.returned, st);
    }
  } else if (frame.fn && ret->value && !quiet_) {
    const Type *retType = frame.fn->sym ? frame.fn->sym->ret : nullptr;
    if (tracked(retType))
      contribute(next_.rets[frame.fn], fitVal(v, retType));
  }
  st.dead = true;
}

void OverflowCheck::notePredicate(const FunctionDecl *fn, const State &whenTrue,
                                  const State &whenFalse) {
  if (quiet_)
    return;
  Pred &pred = next_.preds[fn];
  pred.whenTrue.resize(fn->params.size());
  pred.whenFalse.resize(fn->params.size());
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const VarSymbol *p = fn->params[i].sym;
    if (!p || !isNumberType(p->type) || p->reassigned)
      continue;
    auto value = [&](const State &s) {
      if (s.dead)
        return Num();
      auto found = s.vars.find(p);
      return found == s.vars.end() ? Num() : found->second.num;
    };
    pred.whenTrue[i] = join(pred.whenTrue[i], value(whenTrue));
    pred.whenFalse[i] = join(pred.whenFalse[i], value(whenFalse));
  }
}

//===----------------------------------------------------------------------===//
// Loops
//===----------------------------------------------------------------------===//

// Whether `stmt` yields, which makes whoever iterates the generator decide
// how many passes its loop makes.
bool yields(const Stmt *stmt) {
  bool found = false;
  visitExprs(stmt, [&](const Expr *e) { found |= isa<YieldExpr>(e); }, /*intoFunctions=*/false);
  return found;
}

// Whether `stmt` holds a loop, or a callback that runs where it is passed.
bool containsLoop(const Stmt *stmt) {
  if (!stmt)
    return false;
  switch (stmt->kind()) {
  case Stmt::Kind::While:
  case Stmt::Kind::DoWhile:
  case Stmt::Kind::For:
  case Stmt::Kind::ForOf:
    return true;
  case Stmt::Kind::Block:
    return llvm::any_of(cast<BlockStmt>(stmt)->body, containsLoop);
  case Stmt::Kind::If:
    if (containsLoop(cast<IfStmt>(stmt)->thenStmt) || containsLoop(cast<IfStmt>(stmt)->elseStmt))
      return true;
    break;
  case Stmt::Kind::Labeled:
    return containsLoop(cast<LabeledStmt>(stmt)->body);
  case Stmt::Kind::Switch:
    for (const SwitchCase &c : cast<SwitchStmt>(stmt)->cases)
      if (llvm::any_of(c.body, containsLoop))
        return true;
    break;
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    if (containsLoop(s->block) || containsLoop(s->handler) || containsLoop(s->finalizer))
      return true;
    break;
  }
  default:
    break;
  }
  bool found = false;
  visitExprs(
      stmt,
      [&](const Expr *e) {
        if (auto *call = dyn_cast<CallExpr>(e))
          if (std::optional<size_t> at = callbackArg(call->builtin); at && *at < call->args.size())
            found |= isa<FunctionExpr>(stripCasts(call->args[*at]));
      },
      /*intoFunctions=*/false);
  return found;
}

// Whether a loop with the condition `cond` (null: none) is open-ended: no
// condition, a constant one, or one that reads the input.
bool openEnded(const Expr *cond, const Stmt *body) {
  if (yields(body))
    return false;
  if (!cond)
    return true;
  if (auto *lit = dyn_cast<BoolLitExpr>(stripCasts(cond)))
    return lit->value;
  bool input = false;
  visitExprs(cond, [&](const Expr *e) {
    if (auto *call = dyn_cast<CallExpr>(e))
      input |= call->builtin == Builtin::ReadLine || isFsBuiltin(call->builtin) ||
               isChildBuiltin(call->builtin);
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
           call->builtin == Builtin::ArrSplice))
        found = true;
    } else if (auto *assign = dyn_cast<AssignExpr>(e)) {
      if (auto *index = dyn_cast<IndexExpr>(assign->target);
          index && isVar(index->object) && index->mayAppend)
        found = true;
      if (auto *member = dyn_cast<MemberExpr>(assign->target);
          member && member->name == "length" && isVar(member->object))
        found = true;
    }
  });
  return found;
}

void OverflowCheck::execLoop(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels, State &st) {
  LoopSpec spec;
  spec.loc = stmt->loc();
  spec.labels.assign(labels.begin(), labels.end());
  if (auto *s = dyn_cast<WhileStmt>(stmt)) {
    spec.cond = s->cond;
    spec.openEnded = openEnded(s->cond, s->body);
    spec.simple = !containsLoop(s->body);
    spec.body = [this, s](State &in) { exec(s->body, in); };
    runLoop(spec, st);
    return;
  }
  if (auto *s = dyn_cast<DoWhileStmt>(stmt)) {
    spec.condAfter = s->cond;
    spec.openEnded = openEnded(s->cond, s->body);
    spec.simple = !containsLoop(s->body);
    spec.body = [this, s](State &in) { exec(s->body, in); };
    runLoop(spec, st);
    return;
  }
  if (auto *s = dyn_cast<ForStmt>(stmt)) {
    exec(s->init, st);
    spec.cond = s->cond;
    spec.step = s->step;
    spec.openEnded = openEnded(s->cond, s->body);
    spec.simple = !containsLoop(s->body);
    spec.body = [this, s](State &in) { exec(s->body, in); };
    runLoop(spec, st);
    return;
  }
  auto *s = cast<ForOfStmt>(stmt);
  Val iterable = eval(s->iterable, st);
  const Type *type = s->iterable->type ? s->iterable->type->nonOptional() : nullptr;
  const Type *bound = s->binding.sym ? s->binding.sym->type : nullptr;
  auto *name = dyn_cast<NameExpr>(stripCasts(s->iterable));
  const VarSymbol *iterated = name && st.vars.count(name->var) ? name->var : nullptr;
  Val element = trusted(bound);
  if (type && type->isArray()) {
    element = elementsOf(s->iterable, iterable, type);
    // An array that the loop makes longer goes on with the new elements.
    if (!iterated || !grows(s->body, iterated))
      spec.passes = add(iterable.len, exactly(1));
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
    if (iterated && type && type->isArray())
      e = elementsOf(s->iterable, readVar(iterated, in), type);
    writeVar(s->binding.sym, e, in);
    exec(s->body, in);
  };
  runLoop(spec, st);
}

State OverflowCheck::onePass(const LoopSpec &spec, const State &head, State &exits) {
  ++work_;
  State s = head;
  if (spec.exitAtHead)
    exits = join(exits, s);
  if (spec.cond) {
    State t, f;
    refine(spec.cond, s, t, f);
    exits = join(exits, f);
    s = t;
  }
  Target target;
  target.loop = true;
  target.labels = spec.labels;
  target.quiet = quiet_;
  targets_.push_back(std::move(target));
  if (!s.dead)
    spec.body(s);
  State conts = targets_.back().conts, breaks = targets_.back().breaks;
  targets_.pop_back();
  s = join(s, conts);
  exits = join(exits, breaks);
  if (spec.step && !s.dead)
    eval(spec.step, s);
  if (spec.condAfter && !s.dead) {
    State t, f;
    refine(spec.condAfter, s, t, f);
    exits = join(exits, f);
    s = t;
  }
  return s;
}

State OverflowCheck::rebased(const State &state,
                             llvm::DenseMap<const VarSymbol *, uint64_t> &bases) {
  State r = state;
  for (auto &entry : r.vars) {
    uint64_t &base = bases[entry.first];
    if (!base) {
      base = ++bases_;
      ++bases_;
    }
    entry.second.num.base = base;
    entry.second.num.offLo = entry.second.num.offHi = 0;
    entry.second.len.base = base + 1;
    entry.second.len.offLo = entry.second.len.offHi = 0;
  }
  return r;
}

// How far a pass moved a value from what it was at the head: [offLo, offHi]
// if it only added to it.
std::optional<std::pair<double, double>> stepOf(const Num &end, uint64_t base) {
  if (end.empty() || end.base != base || !base)
    return std::nullopt;
  return std::make_pair(end.offLo, end.offHi);
}

Num OverflowCheck::passCount(const LoopSpec &spec, const State &entry, const State &head,
                             const State &first, const State &firstEnd) {
  if (firstEnd.dead)
    return spec.passes;
  Num best = spec.passes;
  if (!best.empty())
    best = make(Bound{0}, Bound{std::max(best.hi, 0.0), best.hiOrigin, best.hiWhy});
  auto consider = [&](double n, const Bound &limit) {
    if (!(n >= 0) || n == kInf)
      return;
    if (best.empty() || n < best.hi)
      best = make(Bound{0}, Bound{std::floor(n), limit.origin, limit.why});
  };
  for (const auto &entryVar : entry.vars) {
    const VarSymbol *var = entryVar.first;
    auto h = head.vars.find(var), f = first.vars.find(var), e = firstEnd.vars.find(var);
    if (h == head.vars.end() || f == first.vars.end() || e == firstEnd.vars.end())
      continue;
    const Num &start = entryVar.second.num;
    // A counter that moves by the same step on every pass, which the loop's
    // checks keep within bounds.
    if (std::optional<std::pair<double, double>> step = stepOf(e->second.num, f->second.num.base);
        step && step->first == step->second && step->first != 0 && isIntegerType(var->type) &&
        !start.empty()) {
      double s = step->first;
      Limits l = limitsOf(var->type);
      const Num &bounded = h->second.num;
      if (s > 0 && bounded.hi < l.hi - s)
        consider((bounded.hi - start.lo) / s + 1, bounded.high());
      if (s < 0 && bounded.lo > l.lo - s)
        consider((start.hi - bounded.lo) / -s + 1, bounded.low());
    }
    // A length that shrinks on every pass: it cannot go below 0.
    if (std::optional<std::pair<double, double>> step = stepOf(e->second.len, f->second.len.base);
        step && step->second < 0 && !entryVar.second.len.empty())
      consider(entryVar.second.len.hi / -step->second + 1, entryVar.second.len.high());
  }
  return best;
}

void OverflowCheck::runLoop(const LoopSpec &spec, State &st) {
  if (st.dead)
    return;
  const State entry = st;
  // The passes before the last only find the head of the loop: what holds at
  // the start of every pass. Nothing is recorded in them, and values are not
  // wrapped around in them, so that a sum keeps its relation to the head:
  // each pass starts with every value relative to itself, and what a pass
  // adds to a value is its step.
  ++quiet_;
  llvm::DenseMap<const VarSymbol *, uint64_t> bases;

  // 1. Until the head holds what the passes give, every end that moves goes
  //    to the end of its type's range.
  State head = entry;
  State lastHead, lastEnd = deadState();
  bool comesBack = false;
  for (int k = 0; k < (overBudget() ? 2 : 6); ++k) {
    State from = rebased(head, bases);
    State exits = deadState();
    State end = onePass(spec, from, exits);
    if (end.dead)
      break;
    comesBack = true;
    bool moved = false;
    State next = join(entry, end);
    for (auto &headVar : head.vars) {
      const VarSymbol *var = headVar.first;
      auto found = next.vars.find(var);
      if (found == next.vars.end())
        continue;
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
        if (to.empty() || got.empty())
          return;
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
    if (!moved)
      break;
  }
  if (!comesBack) {
    // The body never comes back for another pass.
    --quiet_;
    State exits = deadState();
    onePass(spec, entry, exits);
    st = exits;
    return;
  }

  // 2. Passes from that head bring back the bounds that the checks and the
  //    body keep: `i < n` bounds i.
  for (int k = 0; k < (overBudget() ? 1 : 2); ++k) {
    State from = rebased(head, bases);
    State exits = deadState();
    State end = onePass(spec, from, exits);
    if (end.dead)
      break;
    lastEnd = end;
    lastHead = from;
    State next = join(entry, end);
    for (auto &headVar : head.vars) {
      auto found = next.vars.find(headVar.first);
      if (found == next.vars.end())
        continue;
      Val &h = headVar.second;
      const Val &n = found->second;
      if (!h.num.empty() && !n.num.empty())
        h.num = meet(h.num, n.num);
      if (!h.len.empty() && !n.len.empty())
        h.len = meet(h.len, n.len);
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
    if (moving.origin == Origin::Trusted || moving.origin == Origin::Input)
      return Bound{at, moving.origin, moving.why};
    if (unbounded && additive && !spec.openEnded)
      return Bound{at, Origin::Trusted, nullptr};
    return Bound{at, Origin::Loop, reason(Origin::Loop, spec.loc, "", unbounded)};
  };
  auto stepIn = [&](const VarSymbol *var, bool length) -> std::optional<std::pair<double, double>> {
    auto end = lastEnd.vars.find(var), from = lastHead.vars.find(var);
    if (lastEnd.dead || end == lastEnd.vars.end() || from == lastHead.vars.end())
      return std::nullopt;
    return length ? stepOf(end->second.len, from->second.len.base)
                  : stepOf(end->second.num, from->second.num.base);
  };

  // 3. How many passes the loop makes, and so how far what it adds to on
  //    every pass gets.
  Num passes = passCount(spec, entry, head, lastHead, lastEnd);
  // A loop that its checks end after a few passes, as
  // `while (true) { i++; if (i == 3) break }`: run from the entry, it stops.
  if ((passes.empty() || passes.hi > kUnrollPasses) && spec.simple && !overBudget()) {
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
  if (!passes.empty() && passes.hi <= kUnrollPasses && spec.simple && !overBudget()) {
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
      if (found == all.vars.end())
        continue;
      Val v = found->second;
      v.num.base = v.len.base = 0;
      // Beyond the type: it wraps around. That is this loop's doing unless
      // the value was at the end of its range when the loop started.
      Limits l = limitsOf(headVar.first->type);
      const Num &start = entry.vars.lookup(headVar.first).num;
      if (!v.num.empty() && v.num.hi > l.hi)
        v.num.setHigh(start.empty() || start.hi < l.hi ? limit(v.num.high(), l.hi, false)
                                                       : start.high());
      if (!v.num.empty() && v.num.lo < l.lo)
        v.num.setLow(start.empty() || start.lo > l.lo ? limit(v.num.low(), l.lo, false)
                                                      : start.low());
      headVar.second = v;
    }
  } else {
    for (auto &headVar : head.vars) {
      const VarSymbol *var = headVar.first;
      auto end = lastEnd.vars.find(var);
      if (lastEnd.dead || end == lastEnd.vars.end())
        continue;
      Val &h = headVar.second;
      const Val &start = entry.vars.lookup(var);
      auto grow = [&](Num &n, const Num &startNum, const Num &endNum,
                      std::optional<std::pair<double, double>> step, Limits l) {
        if (n.empty() || startNum.empty() || endNum.empty())
          return;
        if (n.hi >= l.hi && startNum.hi < l.hi) {
          if (step && !passes.empty() && step->second > 0) {
            // A sum of a bounded number of steps.
            // After k passes the head holds the start plus k steps, and the
            // passes bound the states at the head, the start among them.
            double hi = plus(startNum.hi, times(std::max(passes.hi - 1, 0.0), step->second), kInf);
            if (hi <= l.hi)
              n.setHigh(joint(hi, joint(hi, startNum.high(), endNum.high()), passes.high()));
            else
              n.setHigh(limit(joint(hi, endNum.high(), passes.high()), l.hi, false));
          } else {
            n.setHigh(limit(endNum.high(), l.hi, passes.empty(), step && step->second > 0));
          }
        }
        if (n.lo <= l.lo && startNum.lo > l.lo) {
          if (step && !passes.empty() && step->first < 0) {
            double lo = plus(startNum.lo, times(std::max(passes.hi - 1, 0.0), step->first), -kInf);
            if (lo >= l.lo)
              n.setLow(joint(lo, joint(lo, startNum.low(), endNum.low()), passes.high()));
            else
              n.setLow(limit(joint(lo, endNum.low(), passes.high()), l.lo, false));
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
          if (el.num.hi >= l.hi && was.hi < l.hi)
            el.num.setHigh(limit(moved.high(), l.hi, passes.empty()));
          if (el.num.lo <= l.lo && was.lo > l.lo)
            el.num.setLow(limit(moved.low(), l.lo, passes.empty()));
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
      if (!step || n.empty() || start.empty())
        return;
      if (step->first == 0 && step->second == 0) {
        n = start;
        return;
      }
      if (!start.base)
        return;
      n.base = start.base;
      n.offLo = plus(start.offLo, n.lo - start.lo, -kInf);
      n.offHi = plus(start.offHi, n.hi - start.hi, kInf);
    };
    relate(headVar.second.num, e.num, stepIn(headVar.first, false));
    relate(headVar.second.len, e.len, stepIn(headVar.first, true));
  }
  --quiet_;

  // 4. The pass that counts, from the head that holds on every pass.
  State exits = deadState();
  onePass(spec, head, exits);
  st = exits;
}

//===----------------------------------------------------------------------===//
// Conditions
//===----------------------------------------------------------------------===//

// A number written as a literal: `3`, `-1`, `2.5`.
std::optional<double> literalValue(const Expr *expr) {
  expr = stripCasts(expr);
  if (auto *lit = dyn_cast_or_null<IntLitExpr>(expr))
    return double(lit->value);
  if (auto *lit = dyn_cast_or_null<FloatLitExpr>(expr))
    return lit->value;
  if (auto *unary = dyn_cast_or_null<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg)
    if (std::optional<double> v = literalValue(unary->operand))
      return -*v;
  return std::nullopt;
}

std::optional<OverflowCheck::Narrowable> OverflowCheck::narrowable(const Expr *expr,
                                                                   const State &st) const {
  // An int compared as a long or a double is the same value.
  while (auto *conv = dyn_cast_or_null<CastExpr>(expr)) {
    if (!conv->isImplicit() || !conv->operand->type ||
        !conv->operand->type->nonOptional()->isNumeric())
      break;
    expr = conv->operand;
  }
  if (!expr)
    return std::nullopt;
  // `x + 1 < n`, `2 * x <= n`, `x * x <= n`: the variable inside.
  if (auto *bin = dyn_cast<BinaryExpr>(expr);
      bin && (bin->op == BinaryOp::Add || bin->op == BinaryOp::Sub || bin->op == BinaryOp::Mul)) {
    if (bin->op == BinaryOp::Mul) {
      std::optional<Narrowable> a = narrowable(bin->lhs, st), b = narrowable(bin->rhs, st);
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
    if (!c)
      return std::nullopt;
    std::optional<Narrowable> n = narrowable(other, st);
    if (!n || n->square)
      return std::nullopt;
    if (bin->op == BinaryOp::Add) {
      n->offset += *c;
    } else if (bin->op == BinaryOp::Sub) {
      if (constantFirst) { // c - x
        n->scale = -n->scale;
        n->offset = *c - n->offset;
      } else {
        n->offset -= *c;
      }
    } else {
      if (*c == 0)
        return std::nullopt;
      n->scale *= *c;
      n->offset *= *c;
    }
    n->integer = n->integer && *c == std::trunc(*c);
    return n;
  }
  Narrowable n;
  const NameExpr *name = nullptr;
  if (auto *update = dyn_cast<UpdateExpr>(expr)) {
    // After `++k` the value is k; after `k++` it was one less.
    name = dyn_cast<NameExpr>(update->target);
    if (!update->prefix)
      n.offset = update->increment ? -1 : 1;
    if (name && name->var)
      n.integer = isIntegerType(name->var->type);
  } else if (auto *member = dyn_cast<MemberExpr>(expr);
             member && member->name == "length" && hasLength(member->object->type)) {
    name = dyn_cast<NameExpr>(stripCasts(member->object));
    n.length = true;
    n.integer = true;
  } else {
    name = dyn_cast<NameExpr>(expr);
    if (name && name->var)
      n.integer = isIntegerType(name->var->type);
  }
  if (!name || !name->var || !st.vars.count(name->var))
    return std::nullopt;
  auto info = vars_.find(name->var);
  if (info == vars_.end() || info->second.owner != owner_ || info->second.writtenElsewhere)
    return std::nullopt;
  if (!n.length && !isNumberType(name->var->type))
    return std::nullopt;
  n.var = name->var;
  return n;
}

// Narrows what `target` reads in `s`, knowing that its value lies within
// [lower, upper].
void OverflowCheck::constrain(State &s, const Narrowable &target, const Bound &lower,
                              const Bound &upper) {
  if (s.dead)
    return;
  auto found = s.vars.find(target.var);
  if (found == s.vars.end())
    return;
  Num &n = target.length ? found->second.len : found->second.num;
  if (n.empty())
    return;
  auto down = [&](double v) { return target.integer ? std::floor(v) : v; };
  auto up = [&](double v) { return target.integer ? std::ceil(v) : v; };
  if (target.square) {
    if (upper.v == kInf)
      return;
    if (upper.v < 0) {
      s.dead = true;
      return;
    }
    double r = down(std::sqrt(upper.v));
    n = narrowTo(n, Bound{-r, upper.origin, upper.why}, Bound{r, upper.origin, upper.why});
  } else {
    // value = var * scale + offset
    Bound lo = lower, hi = upper;
    lo.v = (lower.v - target.offset) / target.scale;
    hi.v = (upper.v - target.offset) / target.scale;
    if (target.scale < 0)
      std::swap(lo, hi);
    if (std::isnan(lo.v))
      lo.v = -kInf;
    if (std::isnan(hi.v))
      hi.v = kInf;
    lo.v = up(lo.v);
    hi.v = down(hi.v);
    n = narrowTo(n, lo, hi);
  }
  if (n.empty())
    s.dead = true;
}

void OverflowCheck::narrowComparison(const BinaryExpr *bin, const Val &l, const Val &r,
                                     State &whenTrue, State &whenFalse) {
  auto apply = [&](const Expr *side, const Num &other, BinaryOp op) {
    // side `op` other
    std::optional<Narrowable> target = narrowable(side, whenTrue);
    if (!target || other.empty())
      return;
    // Integers that differ differ by at least 1.
    bool integer =
        target->integer && target->scale == 1 && target->offset == std::trunc(target->offset);
    auto below = [&](const Bound &b) {
      return Bound{integer ? std::ceil(b.v) - 1 : b.v, b.origin, b.why};
    };
    auto above = [&](const Bound &b) {
      return Bound{integer ? std::floor(b.v) + 1 : b.v, b.origin, b.why};
    };
    Bound none{-kInf}, top{kInf};
    // `x != c` at one end of what x holds.
    auto exclude = [&](State &s) {
      if (s.dead || other.lo != other.hi || !integer || target->square)
        return;
      auto found = s.vars.find(target->var);
      if (found == s.vars.end())
        return;
      Num &n = target->length ? found->second.len : found->second.num;
      double c = other.lo - target->offset;
      if (n.empty())
        return;
      if (n.lo == c)
        n.lo += 1;
      else if (n.hi == c)
        n.hi -= 1;
      if (n.empty())
        s.dead = true;
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

void OverflowCheck::applyPredicate(const CallExpr *call, State &whenTrue, State &whenFalse) {
  const FunctionDecl *fn = call->func ? call->func->decl : nullptr;
  if (!fn || round_ == 0)
    return;
  auto found = cur_.preds.find(fn);
  if (found == cur_.preds.end())
    return;
  const Pred &pred = found->second;
  for (size_t i = 0; i < call->args.size() && i < fn->params.size(); ++i) {
    std::optional<Narrowable> target = narrowable(call->args[i], whenTrue);
    if (!target || target->length)
      continue;
    auto narrow = [&](State &s, const std::vector<Num> &facts) {
      if (s.dead || i >= facts.size() || facts[i].empty())
        return;
      auto var = s.vars.find(target->var);
      if (var == s.vars.end() || var->second.num.empty())
        return;
      var->second.num = narrowTo(var->second.num, facts[i].lo, facts[i].hi);
      if (var->second.num.empty())
        s.dead = true;
    };
    narrow(whenTrue, pred.whenTrue);
    narrow(whenFalse, pred.whenFalse);
  }
}

void OverflowCheck::refine(const Expr *cond, State &st, State &whenTrue, State &whenFalse) {
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
  // `(k++, k < 3)`: the last one decides.
  if (auto *seq = dyn_cast<SequenceExpr>(cond); seq && !seq->exprs.empty()) {
    for (size_t i = 0; i + 1 < seq->exprs.size(); ++i)
      eval(seq->exprs[i], st);
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
      Val l = eval(bin->lhs, st), r = eval(bin->rhs, st);
      whenTrue = whenFalse = st;
      if (isNumberType(bin->lhs->type) || isNumberType(bin->rhs->type) ||
          isa<MemberExpr>(stripCasts(bin->lhs)) || isa<MemberExpr>(stripCasts(bin->rhs)))
        narrowComparison(bin, l, r, whenTrue, whenFalse);
      return;
    }
    default:
      break;
    }
  }
  if (auto *call = dyn_cast<CallExpr>(cond); call && call->func && call->builtin == Builtin::None) {
    eval(cond, st);
    whenTrue = whenFalse = st;
    applyPredicate(call, whenTrue, whenFalse);
    return;
  }
  eval(cond, st);
  whenTrue = whenFalse = st;
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

Val OverflowCheck::eval(const Expr *expr, State &st, bool wraps) {
  if (!expr || st.dead)
    return Val();
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
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs)
      eval(e, st);
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
                                    ? elementsOf(e.expr, v, type)
                                    : trusted(lit->type && lit->type->isArray() ? lit->type->inner()
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
    return evalCall(cast<CallExpr>(expr), st);
  case Expr::Kind::New:
    return evalNew(cast<NewExpr>(expr), st);
  case Expr::Kind::Conditional: {
    auto *c = cast<ConditionalExpr>(expr);
    State t, f;
    refine(c->cond, st, t, f);
    Val a = eval(c->thenExpr, t, wraps), b = eval(c->elseExpr, f, wraps);
    st = join(t, f);
    if (t.dead)
      return b;
    if (f.dead)
      return a;
    return join(a, b);
  }
  case Expr::Kind::Cast:
    return evalCast(cast<CastExpr>(expr), st, wraps);
  case Expr::Kind::Sequence: {
    Val last;
    for (const Expr *e : cast<SequenceExpr>(expr)->exprs)
      last = eval(e, st, wraps);
    return last;
  }
  case Expr::Kind::OptionalChain:
    return eval(cast<OptionalChainExpr>(expr)->chain, st, wraps);
  case Expr::Kind::Function:
    return Val();
  case Expr::Kind::Yield:
    eval(cast<YieldExpr>(expr)->value, st);
    calledUserCode(st);
    return trusted(expr->type);
  case Expr::Kind::Await:
    // Other tasks run while this one waits, as code a call runs: what the
    // Promise gives is a value of its type.
    eval(cast<AwaitExpr>(expr)->value, st);
    calledUserCode(st);
    return trusted(expr->type);
  }
  return Val();
}

Val OverflowCheck::evalName(const NameExpr *name, State &st) {
  if (!name->var || !tracked(name->var->type))
    return Val();
  return fitVal(readVar(name->var, st), name->type ? name->type : name->var->type);
}

Val OverflowCheck::evalMember(const MemberExpr *member, State &st) {
  Val object = eval(member->object, st);
  const Type *objectType = member->object->type ? member->object->type->nonOptional() : nullptr;
  if (!objectType)
    return Val();
  if (member->name == "length" && hasLength(objectType))
    return numVal(object.len.empty() ? someLength() : object.len);
  if (!tracked(member->type))
    return Val();
  if (objectType->isObject() && !objectType->isAnyObject())
    return readField(objectType, member->name, member->type);
  return trusted(member->type);
}

Val OverflowCheck::evalIndex(const IndexExpr *index, State &st) {
  Val object = eval(index->object, st);
  eval(index->index, st);
  const Type *objectType = index->object->type ? index->object->type->nonOptional() : nullptr;
  if (!objectType || !tracked(index->type))
    return Val();
  if (objectType->isArray())
    return elementsOf(index->object, object, objectType);
  if (objectType->isString())
    return lengthVal(range(0, 1));
  if (!index->field.empty() && objectType->isObject())
    return readField(objectType, index->field, index->type);
  return trusted(index->type);
}

Val OverflowCheck::evalUnary(const UnaryExpr *unary, State &st, bool wraps) {
  const Type *type = unary->type;
  switch (unary->op) {
  case UnaryOp::Neg: {
    // `-2147483648` is a literal.
    if (auto *lit = dyn_cast<IntLitExpr>(unary->operand))
      return numVal(exactly(-double(lit->value)));
    Num a = eval(unary->operand, st, wraps).num;
    if (!isNumberType(type))
      return Val();
    return numVal(arithmetic(unary, BinaryOp::Sub, exactly(0), a, type, wraps, nullptr,
                             unary->operand, nullptr));
  }
  case UnaryOp::Plus: {
    Val v = eval(unary->operand, st, wraps);
    const Type *from = unary->operand->type;
    if (from && from->nonOptional()->isString())
      return numVal(input(type, unary->loc(), "'+' of a string"));
    return numVal(v.num);
  }
  case UnaryOp::BitNot: {
    Num a = eval(unary->operand, st, true).num;
    if (a.empty() || !isNumberType(type))
      return Val();
    Num r = make({-a.hi - 1, a.hiOrigin, a.hiWhy}, {-a.lo - 1, a.loOrigin, a.loWhy});
    return numVal(fit(r, type));
  }
  case UnaryOp::NonNull:
    return eval(unary->operand, st, wraps);
  default:
    eval(unary->operand, st);
    return Val();
  }
}

Num OverflowCheck::bitwise(BinaryOp op, const Num &l, const Num &r, const Type *type) const {
  if (l.empty() || r.empty())
    return Num();
  Limits lim = limitsOf(type);
  // Bits mix: any value of the type, as worrying as the worse side.
  Num full = mixed(lim.lo, lim.hi, l, r);
  bool lPos = l.lo >= 0, rPos = r.lo >= 0;
  auto zero = Bound{0};
  switch (op) {
  case BinaryOp::BitAnd:
    // A mask that is not negative keeps the value within it.
    if (lPos && rPos)
      return make(zero, l.hi < r.hi ? l.high() : r.high());
    if (rPos)
      return make(zero, r.high());
    if (lPos)
      return make(zero, l.high());
    return full;
  case BinaryOp::BitOr:
    if (r.lo == 0 && r.hi == 0)
      return fit(l, type);
    if (l.lo == 0 && l.hi == 0)
      return fit(r, type);
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
      if (l.hi * k <= lim.hi)
        return make({l.lo * k, l.loOrigin, l.loWhy}, {l.hi * k, l.hiOrigin, l.hiWhy});
    }
    return full;
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    unsigned mask = lim.hi > kIntMax ? 63 : 31;
    if (op == BinaryOp::UShr && !lPos) {
      if (r.lo == r.hi && r.lo >= 1 && r.lo < 32)
        return make(zero,
                    {std::floor(4294967295.0 / std::ldexp(1.0, int(r.lo))), l.loOrigin, l.loWhy});
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

Val OverflowCheck::evalBinary(const BinaryExpr *bin, State &st, bool wraps) {
  switch (bin->op) {
  case BinaryOp::LogAnd:
  case BinaryOp::LogOr: {
    State t, f;
    refine(bin, st, t, f);
    return trusted(bin->type);
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
    if (l.len.empty() || r.len.empty())
      return lengthVal(someLength());
    return lengthVal(narrowTo(add(l.len, r.len), 0, kLengthMax));
  }
  if (!isNumberType(type))
    return Val();
  if (bits)
    return numVal(bitwise(bin->op, l.num, r.num, type));
  const VarSymbol *accum = selfAssigned_.lookup(bin);
  if (auto *name = dyn_cast<NameExpr>(stripCasts(bin->lhs));
      !accum && name && accumulators_.contains(name->var))
    accum = name->var;
  return numVal(arithmetic(bin, bin->op, l.num, r.num, type, wraps, bin->lhs, bin->rhs, accum));
}

Num OverflowCheck::arithmetic(const Expr *site, BinaryOp op, const Num &l, const Num &r,
                              const Type *type, bool wraps, const Expr *lhs, const Expr *rhs,
                              const VarSymbol *accum) {
  if (l.empty() || r.empty())
    return Num();
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
  if (exact.empty() || !type->nonOptional()->isInt())
    return fit(exact, type);
  if (exact.lo >= kIntMin && exact.hi <= kIntMax)
    return exact;
  if (!wraps && !quiet_) {
    // Trusted ends count as small: the warning is about the others.
    auto blamed = [](Num n) {
      if (n.loOrigin == Origin::Trusted)
        n.setLow({std::clamp(n.lo, -1.0, 1.0)});
      if (n.hiOrigin == Origin::Trusted)
        n.setHigh({std::clamp(n.hi, -1.0, 1.0)});
      if (n.lo > n.hi)
        std::swap(n.lo, n.hi);
      return n;
    };
    Num exceeding = apply(blamed(l), blamed(r));
    bool constant = (!lhs || isConstantTree(lhs)) && (!rhs || isConstantTree(rhs)) && (lhs || rhs);
    if ((exceeding.lo < kIntMin || exceeding.hi > kIntMax) && !constant) {
      warn(site, l, r, lhs, rhs, exceeding, accum);
      // Reported once: what comes of it is not reported again.
      return range(kIntMin, kIntMax, Origin::Trusted);
    }
  }
  // Before the last pass of a loop: as it is, so that a sum stays a sum.
  if (quiet_)
    return exact;
  // It wraps around, as `| 0` asks for, or the trusted values stay small.
  return range(kIntMin, kIntMax, wraps ? Origin::Proven : Origin::Trusted);
}

Val OverflowCheck::evalCast(const CastExpr *conv, State &st, bool wraps) {
  Val v = eval(conv->operand, st, wraps);
  const Type *from = conv->operand->type, *to = conv->type;
  if (!to || !from)
    return Val();
  if (to->isString())
    return lengthVal(someLength());
  if (from->nonOptional()->isJson() && isNumberType(to))
    return numVal(input(to, conv->loc(), "JSON"));
  if (!isNumberType(to))
    return hasLength(to) ? v : Val();
  if (!isNumberType(from))
    return trusted(to);
  Num n = v.num;
  if (n.empty())
    return Val();
  if (isIntegerType(to) && !isIntegerType(from)) {
    // A double becomes an int as `x | 0` makes one: the fraction goes, and
    // a value beyond the int range wraps around.
    Limits l = limitsOf(to);
    if (n.lo >= l.lo && n.hi <= l.hi)
      return numVal(
          make({std::trunc(n.lo), n.loOrigin, n.loWhy}, {std::trunc(n.hi), n.hiOrigin, n.hiWhy}));
    Bound w = joint(0, n.low(), n.high());
    return numVal(range(l.lo, l.hi, w.origin, w.why));
  }
  return numVal(fit(n, to));
}

Val OverflowCheck::evalObjectLit(const ObjectLitExpr *lit, State &st) {
  const Type *type = lit->type ? lit->type->nonOptional() : nullptr;
  for (const ObjectLitExpr::Property &p : lit->props) {
    if (p.key)
      eval(p.key, st);
    Val v = eval(p.value, st);
    if (!type || !type->isObject() || p.method)
      continue;
    if (p.spread) {
      // The fields it copies keep what they hold.
      const Type *from = p.value->type ? p.value->type->nonOptional() : nullptr;
      if (!from || !from->isObject())
        continue;
      for (const Field &f : from->fields())
        if (type->field(f.name) && tracked(f.type) && !llvm::is_contained(p.replacedFields, f.name))
          storeField(type, f.name, f.type, readField(from, f.name, f.type));
      continue;
    }
    if (const Field *f = p.name.empty() ? nullptr : type->field(p.name))
      storeField(type, p.name, f->type, v);
  }
  return Val();
}

//===----------------------------------------------------------------------===//
// Assignments
//===----------------------------------------------------------------------===//

OverflowCheck::Place OverflowCheck::place(const Expr *target, State &st) {
  Place p;
  p.type = target->type;
  if (auto *name = dyn_cast<NameExpr>(target)) {
    if (name->var) {
      p.kind = Place::Kind::Var;
      p.var = name->var;
      p.type = name->var->type;
    }
    return p;
  }
  if (auto *member = dyn_cast<MemberExpr>(target)) {
    eval(member->object, st);
    const Type *objectType = member->object->type ? member->object->type->nonOptional() : nullptr;
    if (objectType && member->name == "length" && objectType->isArray()) {
      p.kind = Place::Kind::Length;
      if (auto *name = dyn_cast<NameExpr>(stripCasts(member->object)))
        p.var = name->var;
      return p;
    }
    if (objectType && objectType->isObject()) {
      p.kind = Place::Kind::Field;
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
      p.kind = Place::Kind::Element;
      p.objectType = objectType;
      p.append = index->mayAppend;
      if (auto *name = dyn_cast<NameExpr>(stripCasts(index->object)))
        p.var = name->var;
    } else if (objectType && objectType->isObject() && !index->field.empty()) {
      p.kind = Place::Kind::Field;
      p.objectType = objectType;
      p.field = index->field.str();
    }
    return p;
  }
  return p;
}

Val OverflowCheck::read(const Place &p, State &st, const Expr *target) {
  switch (p.kind) {
  case Place::Kind::Var:
    return readVar(p.var, st);
  case Place::Kind::Field:
    return readField(p.objectType, p.field, p.type);
  case Place::Kind::Element: {
    const Expr *array = cast<IndexExpr>(target)->object;
    Val arr = p.var ? readVar(p.var, st) : Val();
    if (!p.var)
      return trusted(p.type);
    return elementsOf(array, arr, p.objectType);
  }
  case Place::Kind::Length: {
    if (!p.var)
      return numVal(someLength());
    return numVal(readVar(p.var, st).len);
  }
  case Place::Kind::None:
    return trusted(p.type);
  }
  return Val();
}

void OverflowCheck::write(const Place &p, const Val &v, State &st) {
  switch (p.kind) {
  case Place::Kind::Var:
    writeVar(p.var, v, st);
    return;
  case Place::Kind::Field:
    storeField(p.objectType, p.field, p.type, v);
    return;
  case Place::Kind::Element: {
    const Type *element = p.objectType->inner();
    contribute(next_.elements[element], fitVal(v, element));
    if (p.var) {
      auto found = st.vars.find(p.var);
      if (found != st.vars.end()) {
        Val &array = found->second;
        if (array.elem)
          array.elem = share(join(*array.elem, fitVal(v, element)));
        if (p.append && !array.len.empty())
          array.len = narrowTo(join(array.len, add(array.len, exactly(1))), 0, kLengthMax);
      }
    }
    return;
  }
  case Place::Kind::Length:
    if (p.var) {
      auto found = st.vars.find(p.var);
      if (found != st.vars.end() && !v.num.empty())
        found->second.len = narrowTo(v.num, 0, kLengthMax);
    }
    return;
  case Place::Kind::None:
    return;
  }
}

Val OverflowCheck::evalAssign(const AssignExpr *assign, State &st) {
  if (!assign->op) {
    Place p = place(assign->target, st);
    Val v = eval(assign->value, st);
    write(p, v, st);
    return v;
  }
  BinaryOp op = *assign->op;
  Place p = place(assign->target, st);
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
      const VarSymbol *accum = p.kind == Place::Kind::Var ? p.var : nullptr;
      result = numVal(arithmetic(assign, op, old.num, v.num, type, false, assign->target,
                                 assign->value, accum));
    }
  }
  write(p, result, st);
  return result;
}

Val OverflowCheck::evalUpdate(const UpdateExpr *update, State &st) {
  Place p = place(update->target, st);
  Val old = read(p, st, update->target);
  const Type *type = update->target->type;
  if (!isNumberType(type))
    return Val();
  Num result = arithmetic(update, update->increment ? BinaryOp::Add : BinaryOp::Sub, old.num,
                          exactly(1), type, false, update->target, nullptr,
                          p.kind == Place::Kind::Var ? p.var : nullptr);
  write(p, numVal(result), st);
  return update->prefix ? numVal(result) : old;
}

//===----------------------------------------------------------------------===//
// Calls
//===----------------------------------------------------------------------===//

Val OverflowCheck::evalCall(const CallExpr *call, State &st) {
  if (call->builtin != Builtin::None)
    return evalBuiltin(call, st);
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member)
    eval(member->object, st);
  else if (!call->func)
    eval(call->callee, st);
  std::vector<Val> args;
  for (const Expr *arg : call->args)
    args.push_back(eval(arg, st));
  const FunctionDecl *target = call->func     ? call->func->decl
                               : call->method ? call->method->decl
                                              : nullptr;
  if (target)
    passArgs(target, call, args);
  for (const CallExpr::UnionMethod &m : call->unionMethods)
    if (m.method && m.method->decl)
      passArgs(m.method->decl, call, args);
  calledUserCode(st);
  if (!target || call->virtualCall || !call->unionMethods.empty())
    return trusted(call->type);
  return returned(target, call->type);
}

Val OverflowCheck::evalNew(const NewExpr *ne, State &st) {
  std::vector<Val> args;
  const auto &argExprs = ne->ctor ? ne->ctor->args : ne->args;
  for (const Expr *arg : argExprs)
    args.push_back(eval(arg, st));
  if (ne->cls) {
    if (ne->ctor && ne->ctor->func && ne->ctor->func->decl)
      passArgs(ne->ctor->func->decl, ne->ctor, args);
    calledUserCode(st);
    return Val();
  }
  const Type *type = ne->type ? ne->type->nonOptional() : nullptr;
  if (type && type->isArray()) {
    Val v;
    v.len = args.empty() || args[0].num.empty() ? exactly(0) : narrowTo(args[0].num, 0, kLengthMax);
    const Type *element = type->inner();
    v.elem = share(isNumberType(element) ? numVal(exactly(0))
                   : hasLength(element)  ? lengthVal(exactly(0))
                                         : Val());
    return v;
  }
  if (type && type->isDict() && !args.empty() && isNumberType(type->valueType()))
    contribute(next_.dicts[type], trusted(type->valueType()));
  return Val();
}

Val OverflowCheck::runCallback(const FunctionDecl *fn, llvm::ArrayRef<Val> args, size_t first,
                               State &st) {
  Frame frame;
  frame.fn = fn;
  frame.callback = true;
  frame.returned = deadState();
  frame.targets = targets_.size();
  frame.quiet = quiet_;
  frames_.push_back(std::move(frame));
  for (size_t i = first; i < fn->params.size(); ++i) {
    const VarSymbol *p = fn->params[i].sym;
    if (!p || !tracked(p->type))
      continue;
    writeVar(p, i - first < args.size() ? args[i - first] : trusted(p->type), st);
  }
  exec(fn->body, st);
  Frame done = std::move(frames_.back());
  frames_.pop_back();
  st = join(st, done.returned);
  return done.result;
}

Val OverflowCheck::evalCallback(const CallExpr *call, State &st) {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  const Expr *receiver = member ? member->object : nullptr;
  size_t at = *callbackArg(call->builtin);
  if (call->builtin == Builtin::ArrayFrom)
    receiver = call->args.empty() ? nullptr : call->args[0];
  Val array = receiver ? eval(receiver, st) : Val();
  const Type *arrayType = receiver && receiver->type ? receiver->type->nonOptional() : nullptr;
  Val init;
  bool hasInit = false;
  for (size_t i = 0; i < call->args.size(); ++i) {
    if (i == at || (call->builtin == Builtin::ArrayFrom && i == 0))
      continue;
    Val v = eval(call->args[i], st);
    if ((call->builtin == Builtin::ArrReduce || call->builtin == Builtin::ArrReduceRight) &&
        i == 1) {
      init = v;
      hasInit = true;
    }
  }
  Val element;
  Num passes;
  if (call->builtin == Builtin::MapForEach) {
    const Type *value = arrayType && arrayType->isDict() ? arrayType->valueType() : nullptr;
    if (std::optional<Val> v = summary(cur_.dicts, arrayType))
      element = fitVal(*v, value);
    else
      element = trusted(value);
    passes = someLength();
  } else if (arrayType && arrayType->isArray()) {
    element = elementsOf(receiver, array, arrayType);
    passes = array.len.empty() ? someLength() : array.len;
  } else if (arrayType && arrayType->isString()) {
    element = lengthVal(range(1, 2));
    passes = array.len;
  } else {
    element = trusted(nullptr);
  }
  Val indexVal = numVal(
      passes.empty() ? make({0}, {kLengthMax - 1, Origin::Trusted})
                     : make({0}, {std::max(passes.hi - 1, 0.0), passes.hiOrigin, passes.hiWhy}));
  const Expr *arg = at < call->args.size() ? stripCasts(call->args[at]) : nullptr;
  auto *fe = dyn_cast_or_null<FunctionExpr>(arg);
  if (!fe || !inlined_.contains(fe->fn)) {
    // A function by name, called for each element.
    const FunctionDecl *named = nullptr;
    if (auto *name = dyn_cast_or_null<NameExpr>(arg)) {
      if (name->func)
        named = name->func->decl;
      else if (name->var)
        named = name->var->function ? name->var->function : name->var->lambda;
    }
    if (arg)
      eval(call->args[at], st);
    if (named)
      passArgs(named, nullptr, {element, indexVal, array});
    calledUserCode(st);
    Val result = named ? returned(named, named->sym ? named->sym->ret : nullptr) : Val();
    if (call->builtin == Builtin::ArrMap || call->builtin == Builtin::ArrayFrom) {
      Val v;
      v.len = passes.empty() ? someLength() : passes;
      v.elem = share(
          named ? result
                : trusted(call->type && call->type->isArray() ? call->type->inner() : nullptr));
      return v;
    }
    return trusted(call->type);
  }
  const FunctionDecl *fn = fe->fn;
  bool reduce = call->builtin == Builtin::ArrReduce || call->builtin == Builtin::ArrReduceRight;
  const VarSymbol *acc = reduce && !fn->params.empty() ? fn->params[0].sym : nullptr;
  if (acc && tracked(acc->type))
    writeVar(acc, hasInit ? init : element, st);
  Val result;
  LoopSpec spec;
  spec.loc = call->loc();
  spec.exitAtHead = true;
  spec.passes = passes.empty() ? passes : add(passes, exactly(1));
  spec.simple = !containsLoop(fn->body);
  spec.body = [&, fn, acc](State &in) {
    std::vector<Val> args;
    size_t first = 0;
    if (call->builtin == Builtin::ArrSortWith) {
      args = {element, element};
    } else if (call->builtin == Builtin::MapForEach) {
      args = {
          element,
          trusted(fn->params.size() > 1 && fn->params[1].sym ? fn->params[1].sym->type : nullptr),
          Val()};
    } else {
      args = {element, indexVal, array};
    }
    if (acc)
      first = 1;
    Val r = runCallback(fn, args, first, in);
    if (acc && tracked(acc->type))
      writeVar(acc, r, in);
    result = r;
  };
  runLoop(spec, st);
  switch (call->builtin) {
  case Builtin::ArrMap:
  case Builtin::ArrayFrom: {
    Val v;
    v.len = passes.empty() ? someLength() : passes;
    v.elem = share(result);
    return v;
  }
  case Builtin::ArrFilter: {
    Val v;
    v.len = passes.empty() ? someLength() : make({0}, passes.high());
    v.elem = share(element);
    return v;
  }
  case Builtin::ArrFlatMap: {
    Val v;
    v.len = someLength();
    v.elem =
        result.elem
            ? result.elem
            : share(trusted(call->type && call->type->isArray() ? call->type->inner() : nullptr));
    return v;
  }
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight: {
    Val v = acc ? readVar(acc, st) : trusted(call->type);
    if (acc)
      st.vars.erase(acc);
    return fitVal(v, call->type);
  }
  case Builtin::ArrFind:
  case Builtin::ArrFindLast:
    return fitVal(element, call->type);
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLastIndex:
    return numVal(passes.empty()
                      ? make({-1}, {kLengthMax - 1, Origin::Trusted})
                      : make({-1}, {std::max(passes.hi - 1, -1.0), passes.hiOrigin, passes.hiWhy}));
  case Builtin::ArrSortWith:
    return array;
  default:
    return Val();
  }
}

Val OverflowCheck::jsonVal(const Type *type, const Reason *why) {
  if (!type)
    return Val();
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
void OverflowCheck::jsonValues(const Type *type, const Reason *why,
                               llvm::SmallPtrSetImpl<const Type *> &seen) {
  if (!type)
    return;
  type = type->nonOptional();
  if (type->isArray()) {
    jsonValues(type->inner(), why, seen);
    return;
  }
  if (type->isUnion()) {
    for (const Type *m : type->members())
      jsonValues(m, why, seen);
    return;
  }
  if (type->isDict()) {
    contribute(next_.dicts[type], jsonVal(type->valueType(), why));
    jsonValues(type->valueType(), why, seen);
    return;
  }
  if (!type->isObject() || !seen.insert(type).second)
    return;
  for (const Field &f : type->fields()) {
    if (tracked(f.type))
      storeField(type, f.name, f.type, jsonVal(f.type, why));
    jsonValues(f.type, why, seen);
  }
}

Val OverflowCheck::evalMath(const CallExpr *call, State &st) {
  std::vector<Num> args;
  for (const Expr *arg : call->args) {
    Val v = eval(arg, st);
    // Math.max(...a): the elements of a.
    const Type *type = arg->type ? arg->type->nonOptional() : nullptr;
    if (type && type->isArray())
      v = elementsOf(arg, v, type);
    args.push_back(v.num);
  }
  const Type *type = call->type;
  auto arg = [&](size_t i) { return i < args.size() ? args[i] : Num(); };
  Num x = arg(0);
  auto monotone = [&](double (*f)(double), double lo, double hi) {
    if (x.empty())
      return Num();
    double a = f(std::clamp(x.lo, lo, hi)), b = f(std::clamp(x.hi, lo, hi));
    if (std::isnan(a) || std::isnan(b))
      return range(-kInf, kInf);
    return make({a, x.loOrigin, x.loWhy}, {b, x.hiOrigin, x.hiWhy});
  };
  Num r;
  switch (call->builtin) {
  case Builtin::MathFloor:
  case Builtin::MathCeil:
  case Builtin::MathRound:
  case Builtin::MathTrunc: {
    if (x.empty())
      return Val();
    r = make({std::floor(x.lo), x.loOrigin, x.loWhy}, {std::ceil(x.hi), x.hiOrigin, x.hiWhy});
    // An integer result must fit, or the program stops (see emitMathCall).
    Limits l = limitsOf(type);
    if (isIntegerType(type))
      r = narrowTo(r, l.lo, l.hi);
    return numVal(r);
  }
  case Builtin::MathAbs:
    if (x.empty())
      return Val();
    r = make({x.lo >= 0   ? x.lo
              : x.hi <= 0 ? -x.hi
                          : 0},
             std::fabs(x.lo) > std::fabs(x.hi) ? Bound{-x.lo, x.loOrigin, x.loWhy} : x.high());
    // Math.abs of the smallest int is itself.
    if (isIntegerType(type) && r.hi > limitsOf(type).hi)
      r = range(limitsOf(type).lo, limitsOf(type).hi, x.loOrigin, x.loWhy);
    return numVal(r);
  case Builtin::MathSign:
    return numVal(range(-1, 1));
  case Builtin::MathMin:
  case Builtin::MathMax: {
    bool isMin = call->builtin == Builtin::MathMin;
    if (args.empty())
      return numVal(exactly(isMin ? kInf : -kInf));
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
    return numVal(fit(best, type));
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
    return trusted(type);
  }
}

Val OverflowCheck::evalBuiltin(const CallExpr *call, State &st) {
  if (callbackArg(call->builtin))
    return evalCallback(call, st);
  if (isMathBuiltin(call->builtin))
    return evalMath(call, st);
  auto *member = dyn_cast<MemberExpr>(call->callee);
  const Expr *receiverExpr = member ? member->object : nullptr;
  Val receiver = receiverExpr ? eval(receiverExpr, st) : Val();
  const Type *receiverType =
      receiverExpr && receiverExpr->type ? receiverExpr->type->nonOptional() : nullptr;
  std::vector<Val> args;
  for (const Expr *arg : call->args)
    args.push_back(eval(arg, st));
  const Type *type = call->type;
  auto arg = [&](size_t i) { return i < args.size() ? args[i] : Val(); };
  auto length = [&]() { return receiver.len.empty() ? someLength() : receiver.len; };
  // The length of the receiver, after the call changed it.
  auto setLength = [&](Num len) {
    auto *name = dyn_cast_or_null<NameExpr>(stripCasts(receiverExpr));
    if (!name || !name->var)
      return;
    auto found = st.vars.find(name->var);
    if (found != st.vars.end() && !len.empty())
      found->second.len = narrowTo(len, 0, kLengthMax);
  };
  switch (call->builtin) {
  case Builtin::ParseInt:
    return numVal(input(type, call->loc(), "parseInt"));
  case Builtin::ParseFloat:
    return numVal(input(type, call->loc(), "parseFloat"));
  case Builtin::NumberOf: {
    const Type *from =
        call->args.empty() || !call->args[0]->type ? nullptr : call->args[0]->type->nonOptional();
    if (from && from->isString())
      return numVal(input(type, call->loc(), "Number()"));
    if (from && isNumberType(from))
      return numVal(arg(0).num);
    if (from && from->isBool())
      return numVal(range(0, 1));
    return trusted(type);
  }
  case Builtin::JsonParseAs:
  case Builtin::FromJson: {
    const Reason *why = reason(Origin::Input, call->loc(), "JSON");
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
        values = join(values, elementsOf(call->args[i], args[i], call->args[i]->type));
      } else {
        values = join(values, args[i]);
        ++count;
      }
    }
    storeElements(receiverExpr, receiverType, values, st);
    Num len = spreads ? make({length().lo + count}, {kLengthMax, Origin::Trusted})
                      : add(length(), exactly(count));
    setLength(len);
    return numVal(narrowTo(len, 0, kLengthMax));
  }
  case Builtin::ArrPop:
  case Builtin::ArrShift: {
    Num len = length();
    setLength(narrowTo(add(len, exactly(-1)), 0, kLengthMax));
    return fitVal(elementsOf(receiverExpr, receiver, receiverType), type);
  }
  case Builtin::ArrAt:
    return fitVal(elementsOf(receiverExpr, receiver, receiverType), type);
  case Builtin::ArrSlice: {
    Val v;
    v.len = make({0}, length().high());
    v.elem = share(elementsOf(receiverExpr, receiver, receiverType));
    return v;
  }
  case Builtin::ArrConcat: {
    Val v;
    Val element = elementsOf(receiverExpr, receiver, receiverType);
    Num len = length();
    for (size_t i = 0; i < args.size(); ++i) {
      const Type *t = call->args[i]->type ? call->args[i]->type->nonOptional() : nullptr;
      if (t && t->isArray()) {
        element = join(element, elementsOf(call->args[i], args[i], t));
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
    storeElements(receiverExpr, receiverType, arg(0), st);
    return receiver;
  case Builtin::ArrSplice: {
    Val inserted;
    for (size_t i = 2; i < args.size(); ++i)
      inserted = join(inserted, args[i]);
    if (!isBottom(inserted))
      storeElements(receiverExpr, receiverType, inserted, st);
    Num len = length();
    double removed =
        args.size() > 1 && !args[1].num.empty() ? std::max(args[1].num.hi, 0.0) : len.hi;
    double added = double(args.size() > 2 ? args.size() - 2 : 0);
    setLength(make({std::max(len.lo - removed, 0.0)}, {len.hi + added, len.hiOrigin, len.hiWhy}));
    Val v;
    v.len = make({0}, len.high());
    v.elem = share(elementsOf(receiverExpr, receiver, receiverType));
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
    if (std::optional<Val> v = summary(cur_.dicts, receiverType))
      return fitVal(*v, value);
    return trusted(value);
  }
  case Builtin::MapSet:
  case Builtin::RecordSet:
  case Builtin::RecordNullishSet: {
    if (receiverType && receiverType->isDict() && args.size() > 1)
      contribute(next_.dicts[receiverType], fitVal(args[1], receiverType->valueType()));
    if (call->builtin == Builtin::RecordNullishSet) {
      const Type *value =
          receiverType && receiverType->isDict() ? receiverType->valueType() : nullptr;
      std::optional<Val> v = summary(cur_.dicts, receiverType);
      return join(v ? fitVal(*v, value) : trusted(value), arg(1));
    }
    if (call->builtin == Builtin::RecordSet)
      return arg(1);
    return receiver;
  }
  case Builtin::MapSize:
    return numVal(someLength());
  case Builtin::DictNew:
  case Builtin::DictGroupBy:
    if (type && type->isDict() && isNumberType(type->valueType()) && !args.empty())
      contribute(next_.dicts[type], trusted(type->valueType()));
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
    v.elem = share(trusted(element));
    return v;
  }
  default:
    break;
  }
  // Methods of strings that give strings, and the rest.
  if (call->builtin >= Builtin::RegExpNew && call->builtin <= Builtin::StrSplitRegex)
    calledUserCode(st); // a replacer function runs
  if (isFsBuiltin(call->builtin) || isChildBuiltin(call->builtin))
    mayThrow(st);
  return trusted(type);
}

//===----------------------------------------------------------------------===//
// Warnings
//===----------------------------------------------------------------------===//

// The text of an expression as written, as far as it can be rebuilt.
std::string exprText(const Expr *expr) {
  expr = stripCasts(expr);
  if (!expr)
    return "...";
  switch (expr->kind()) {
  case Expr::Kind::Name:
    return cast<NameExpr>(expr)->name.str();
  case Expr::Kind::IntLit: {
    auto *lit = cast<IntLitExpr>(expr);
    return lit->text.empty() ? std::to_string(lit->value) : lit->text.str();
  }
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(expr);
    return exprText(member->object) + "." + member->name.str();
  }
  case Expr::Kind::Binary:
    if (!cast<BinaryExpr>(expr)->source.empty())
      return cast<BinaryExpr>(expr)->source.str();
    return exprText(cast<BinaryExpr>(expr)->lhs) + " " + opSpelling(cast<BinaryExpr>(expr)->op) +
           " " + exprText(cast<BinaryExpr>(expr)->rhs);
  case Expr::Kind::Assign: {
    auto *assign = cast<AssignExpr>(expr);
    if (!assign->source.empty())
      return assign->source.str();
    return exprText(assign->target) + " " + (assign->op ? opSpelling(*assign->op) : "") + "= " +
           exprText(assign->value);
  }
  case Expr::Kind::Update: {
    auto *update = cast<UpdateExpr>(expr);
    std::string op = update->increment ? "++" : "--";
    return update->prefix ? op + exprText(update->target) : exprText(update->target) + op;
  }
  case Expr::Kind::Unary: {
    auto *unary = cast<UnaryExpr>(expr);
    return std::string(opSpelling(unary->op)) + exprText(unary->operand);
  }
  case Expr::Kind::Call:
    return exprText(cast<CallExpr>(expr)->callee) + "(...)";
  case Expr::Kind::Index:
    return exprText(cast<IndexExpr>(expr)->object) + "[...]";
  default:
    return "...";
  }
}

// A value named in a message: 'x', 'p.count', or a description.
std::string subject(const Expr *expr) {
  expr = stripCasts(expr);
  if (!expr)
    return "the value";
  if (isa<NameExpr>(expr) || isa<MemberExpr>(expr))
    return "'" + exprText(expr) + "'";
  if (auto *index = dyn_cast<IndexExpr>(expr))
    return "the element of '" + exprText(index->object) + "'";
  if (auto *call = dyn_cast<CallExpr>(expr))
    return "the result of '" + exprText(call) + "'";
  return "'" + exprText(expr) + "'";
}

std::string numberText(double v) {
  if (std::fabs(v) == kInf)
    return v > 0 ? "any size" : "any negative size";
  char text[64];
  if (std::fabs(v) < 1e21)
    std::snprintf(text, sizeof text, "%.0f", v);
  else
    std::snprintf(text, sizeof text, "%g", v);
  return text;
}

std::string OverflowCheck::line(llvm::SMLoc loc, llvm::SMLoc from) const {
  llvm::SourceMgr &sources = diags_.sourceMgr();
  unsigned id = sources.FindBufferContainingLoc(loc);
  if (!id)
    return "an unknown line";
  std::string text = "line " + std::to_string(sources.getLineAndColumn(loc, id).first);
  if (id != sources.FindBufferContainingLoc(from))
    text += " of " +
            llvm::sys::path::filename(sources.getMemoryBuffer(id)->getBufferIdentifier()).str();
  return text;
}

std::string OverflowCheck::inputText(const Reason *why, llvm::SMLoc from) const {
  if (!why)
    return "outside the program";
  if (!why->calls.empty()) {
    std::string text = inputText(why->via, from);
    if (why->calls.size() == 1)
      return text + ", passed by the call on " + line(why->calls[0], from);
    text += ", passed by the calls on lines ";
    for (size_t i = 0; i < why->calls.size() && i < 4; ++i) {
      if (i)
        text += i + 1 == why->calls.size() ? " and " : ", ";
      std::string l = line(why->calls[i], from);
      text += llvm::StringRef(l).drop_front(5).str();
    }
    if (why->calls.size() > 4)
      text += " and others";
    return text;
  }
  return why->what + " on " + line(why->loc, from);
}

std::string OverflowCheck::fixText(const Expr *site, const VarSymbol *accum) const {
  if (accum) {
    auto info = vars_.find(accum);
    const VarDeclarator *decl = info != vars_.end() ? info->second.decl : nullptr;
    if (decl && !decl->typeAnn && decl->init && !info->second.isConst) {
      std::string init = isConstantTree(decl->init) ? exprText(decl->init) : "...";
      return "declare it as 'let " + accum->name.str() + ": long = " + init + "', or as a double";
    }
    if (decl && decl->typeAnn)
      return "declare '" + accum->name.str() + "' as 'long' or 'double'";
  }
  // A field that adds up: `this.total += x`.
  const Expr *target = nullptr;
  if (auto *assign = dyn_cast<AssignExpr>(site); assign && assign->op)
    target = assign->target;
  else if (auto *update = dyn_cast<UpdateExpr>(site))
    target = update->target;
  if (auto *member = dyn_cast_or_null<MemberExpr>(target))
    return "make the field '" + member->name.str() + "' a 'long' or a 'double'";
  auto init = inits_.find(site);
  if (init != inits_.end() && init->second.first && !init->second.first->typeAnn &&
      init->second.first->sym) {
    const VarDeclarator *decl = init->second.first;
    return std::string("compute it as a long: '") + (init->second.second ? "const " : "let ") +
           decl->name.str() + ": long = " + exprText(site) +
           "', check the value before, or write '| 0' if wrapping is meant";
  }
  return "check the value before, compute it as a 'long' or a 'double', or write '| 0' if "
         "wrapping is meant";
}

void OverflowCheck::warn(const Expr *site, const Num &l, const Num &r, const Expr *lhs,
                         const Expr *rhs, const Num &exceeding, const VarSymbol *accum) {
  if (!reporting_ || quiet_)
    return;
  // The end that goes beyond int, and where it comes from.
  bool top = exceeding.hi > kIntMax, bottom = exceeding.lo < kIntMin;
  Bound culprit = top && bottom ? joint(0, exceeding.low(), exceeding.high())
                  : top         ? exceeding.high()
                                : exceeding.low();
  // The operand it comes from.
  const Expr *subjectExpr = nullptr;
  // What a sum adds is to blame rather than the sum.
  auto operands = accum ? std::array{std::pair{&r, rhs}, std::pair{&l, lhs}}
                        : std::array{std::pair{&l, lhs}, std::pair{&r, rhs}};
  for (int exact = 1; exact >= 0 && !subjectExpr; --exact)
    for (auto [n, e] : operands) {
      if (!e || subjectExpr)
        continue;
      for (const Bound &b : {n->low(), n->high()})
        if (b.origin == culprit.origin && (!exact || b.why == culprit.why))
          subjectExpr = e;
    }
  std::string why;
  llvm::SMLoc loc = site->loc();
  if (culprit.origin == Origin::Input) {
    why = subject(subjectExpr) + " comes from " + inputText(culprit.why, loc);
  } else if (culprit.origin == Origin::Loop) {
    const Reason *loop = culprit.why;
    while (loop && !loop->calls.empty())
      loop = loop->via;
    why = subject(subjectExpr) + " grows with every pass of the loop on " +
          (loop ? line(loop->loc, loc) : std::string("an unknown line")) +
          (loop && loop->unbounded ? ", which has no bound" : "");
    if (culprit.why && !culprit.why->calls.empty())
      why += ", passed by the call on " + line(culprit.why->calls[0], loc);
  } else {
    why = "it may reach " + numberText(top ? exceeding.hi : exceeding.lo);
  }
  warnings_.push_back(
      {loc, "'" + exprText(site) + "' may overflow int: " + why + "; " + fixText(site, accum)});
}

} // namespace

void checkIntOverflow(SourceFile &program, Diagnostics &diags) {
  OverflowCheck(program, diags).run();
}

} // namespace tz
