#include "compiler/overflow/builtin_evaluator/builtin_evaluator.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/facts/facts.h"
#include "compiler/library/decimal/decimal_literal/decimal_literal.h"
#include "compiler/overflow/call_ranges/call_ranges.h"
#include "compiler/overflow/evaluator/evaluator.h"
#include "compiler/overflow/executor/executor.h"
#include "compiler/overflow/loop_ranges/loop_ranges.h"
#include "compiler/overflow/narrowing/narrowing.h"
#include "compiler/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/overflow/program_scan/program_scan.h"
#include "compiler/overflow/slots/slots.h"
#include "compiler/overflow/value_range/value_range.h"
#include "compiler/program/types/types.h"
#include "runtime/library/date/date/date.h"
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

using llvm::dyn_cast;

using llvm::dyn_cast_or_null;

namespace {

constexpr double kMaxTime = 8.64e15;

constexpr double kDay = 86400000;

constexpr double kFirstYear = -292275055, kLastYear = 292278994;

const double kPartLimits[7][2] = {
    {kFirstYear + 1, kLastYear - 1}, {0, 11}, {1, 31}, {0, 23}, {0, 59}, {0, 59}, {0, 999}};

double monthStart(double months) {
  if (!(std::fabs(months) < 1e15)) {
    return months * 30.436875;
  }
  auto m = int64_t(months);
  int64_t y = (m >= 0 ? m : m - 11) / 12;
  int64_t month = m - y * 12;
  y -= month <= 1;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * ((month + 10) % 12) + 2) / 5;

  return double(era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468);
}

Num dateTime(const std::array<Num, 7> &parts) {
  Num months = add(multiply(parts[0], exactly(12)), parts[1]);
  Num days = make({monthStart(months.lo), months.loOrigin, months.loWhy},
                  {monthStart(months.hi), months.hiOrigin, months.hiWhy});
  Num time = multiply(add(days, subtract(parts[2], exactly(1))), exactly(kDay));
  const double scale[] = {3600000, 60000, 1000, 1};
  for (size_t i = 3; i < 7; ++i) {
    time = add(time, multiply(parts[i], exactly(scale[i - 3])));
  }

  return time;
}

}

void BuiltinEvaluator::checkDateParts(const CallExpr *call, const std::vector<Val> &args) {
  bool setter = call->builtin == Builtin::DateSetUTC || call->builtin == Builtin::DateSetLocal;
  bool local = call->builtin == Builtin::DateFromFields || call->builtin == Builtin::DateSetLocal;
  std::array<Num, 7> nums = {exactly(1970), exactly(0), exactly(1), exactly(0),
                             exactly(0),    exactly(0), exactly(0)};
  size_t first = 0, from = 0;
  if (setter) {
    auto *field = call->args.empty() ? nullptr : dyn_cast<IntLitExpr>(call->args[0]);
    if (!field) {
      return;
    }
    nums = {range(kFirstYear, kLastYear, Origin::Trusted),
            range(0, 11),
            range(1, 31),
            range(0, 23),
            range(0, 59),
            range(0, 59),
            range(0, 999)};
    first = field->value <= TZ_DATE_DATE ? size_t(field->value) : size_t(field->value) - 1;
    from = 1;
  }
  std::vector<DatePart> parts;
  for (size_t i = from; i < call->args.size() && i < args.size() && first + i - from < 7; ++i) {
    size_t k = first + i - from;
    if (args[i].num.empty()) {
      return;
    }
    nums[k] = args[i].num;
    parts.push_back({call->args[i], args[i].num, kPartLimits[k][0], kPartLimits[k][1]});
  }
  if (!setter && nums[0].lo <= 99 && nums[0].hi >= 0) {
    nums[0] = join(nums[0], range(1900, 1999));
  }
  double edge = double(std::numeric_limits<int64_t>::max());
  check_.overflowWarnings().checkDate(dateTime(nums), local ? edge - kDay : edge, parts);
}

BuiltinEvaluator::BuiltinEvaluator(OverflowCheck &check) : check_(check) {}

Val BuiltinEvaluator::evalBuiltin(const CallExpr *call, State &st) {
  if (callbackArg(call->builtin)) {
    return check_.callRanges().evalCallback(call, st);
  }
  if (isMathBuiltin(call->builtin)) {
    return evalMath(call, st);
  }
  auto *member = dyn_cast<MemberExpr>(call->callee);
  const Expr *receiverExpr = member ? member->object : nullptr;
  Val receiver = receiverExpr ? check_.evaluator().eval(receiverExpr, st) : Val();
  const Type *receiverType =
      receiverExpr && receiverExpr->type ? receiverExpr->type->nonOptional() : nullptr;
  std::vector<Val> args;
  for (const Expr *arg : call->args) {
    args.push_back(check_.evaluator().eval(arg, st));
  }
  const Type *type = call->type;
  auto arg = [&](size_t i) { return i < args.size() ? args[i] : Val(); };
  auto length = [&]() { return receiver.len.empty() ? someLength() : receiver.len; };
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
  case Builtin::DataViewGet:
  case Builtin::BufferRead: {
    if (type->isResult()) {
      Val result;
      result.elem =
          share(numVal(check_.overflowWarnings().input(type->okType(), call->loc(), "the bytes")));
      return result;
    }
    int64_t lo, hi;
    if (!call->args.empty() && llvm::isa<IntLitExpr>(call->args[0]) &&
        Type::bytesType(Type::Bytes(llvm::cast<IntLitExpr>(call->args[0])->value))
            ->elementRange(lo, hi)) {
      return numVal(range(double(lo), double(hi)));
    }
    return check_.overflowWarnings().trusted(type);
  }
  case Builtin::ResultUnwrapOr:
    if (receiver.elem && receiverType && receiverType->isResult()) {
      return join(*receiver.elem, arg(0));
    }
    return check_.overflowWarnings().trusted(type);
  case Builtin::ParseInt:
  case Builtin::ParseFloat: {
    Val result;
    result.elem = share(numVal(check_.overflowWarnings().input(
        type->isResult() ? type->okType() : type, call->loc(),
        call->builtin == Builtin::ParseInt ? "parseInt" : "parseFloat")));
    return result;
  }
  case Builtin::NumberOf: {
    const Type *from =
        call->args.empty() || !call->args[0]->type ? nullptr : call->args[0]->type->nonOptional();
    if (from && from->isString()) {
      Val result;
      result.elem = share(numVal(check_.overflowWarnings().input(
          type->isResult() ? type->okType() : type, call->loc(), "Number()")));
      return result;
    }
    if (from && from->isInteger()) {
      check_.overflowWarnings().checkConversion(call, call->args[0], arg(0).num, type);
      return numVal(arg(0).num);
    }
    if (from && from->isDecimal()) {
      Num x = arg(0).num;
      return x.empty() ? check_.overflowWarnings().trusted(type)
                       : numVal(narrowTo(x, -kDoubleMax, kDoubleMax));
    }
    if (from && isNumberType(from)) {
      return numVal(arg(0).num);
    }
    if (from && from->isBool()) {
      return numVal(range(0, 1));
    }
    return check_.overflowWarnings().trusted(type);
  }
  case Builtin::NumConvert: {
    if (type->isDecimal() && receiverType && receiverType->isInteger() && !receiver.num.empty()) {
      Num x = receiver.num;
      bool exact = std::fabs(x.lo) <= 9007199254740992.0 && std::fabs(x.hi) <= 9007199254740992.0;
      return numVal(exact ? x : widenedDecimal(x));
    }
    if (!receiverType || !isNumberType(receiverType) || type->isDecimal() ||
        receiverType->isDecimal()) {
      return check_.overflowWarnings().trusted(type);
    }
    Num x = receiver.num;
    if (x.empty()) {
      return Val();
    }
    if (!check_.overflowWarnings().checkConversion(call, receiverExpr, x, type)) {
      Limits l = limitsOf(type);
      return numVal(range(l.lo, l.hi, Origin::Trusted));
    }
    return numVal(type->isInteger() ? narrowTo(x, limitsOf(type).lo, limitsOf(type).hi) : x);
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
        values =
            join(values, check_.slots().elementsOf(call->args[i], args[i], call->args[i]->type));
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
    return check_.overflowWarnings().fitVal(
        check_.slots().elementsOf(receiverExpr, receiver, receiverType), type);
  }
  case Builtin::ArrAt:
    return check_.overflowWarnings().fitVal(
        check_.slots().elementsOf(receiverExpr, receiver, receiverType), type);
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
      check_.contribute(check_.next_.dicts[receiverType],
                        check_.overflowWarnings().fitVal(args[1], receiverType->valueType()));
    }
    if (call->builtin == Builtin::RecordNullishSet) {
      const Type *value =
          receiverType && receiverType->isDict() ? receiverType->valueType() : nullptr;
      std::optional<Val> v = check_.summary(check_.cur_.dicts, receiverType);
      return join(v ? check_.overflowWarnings().fitVal(*v, value)
                    : check_.overflowWarnings().trusted(value),
                  arg(1));
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
      check_.contribute(check_.next_.dicts[type],
                        check_.overflowWarnings().trusted(type->valueType()));
    }
    return Val();
  case Builtin::DateNew:
  case Builtin::DateSetTime:
  case Builtin::DateGetTime:
    return check_.overflowWarnings().trusted(type);
  case Builtin::DateFromFields:
  case Builtin::DateUTC:
  case Builtin::DateSetUTC:
  case Builtin::DateSetLocal:
    checkDateParts(call, args);
    return check_.overflowWarnings().trusted(type);
  case Builtin::DateNow:
  case Builtin::PerformanceNow:
    return numVal(range(0, kMaxTime, Origin::Trusted));
  case Builtin::DateParse: {
    Val result;
    result.elem = share(numVal(range(-kMaxTime, kMaxTime, Origin::Trusted)));
    return result;
  }
  case Builtin::DateGetUTC:
  case Builtin::DateGetLocal: {
    auto *field = call->args.empty() ? nullptr : dyn_cast<IntLitExpr>(call->args[0]);
    switch (field ? int(field->value) : -1) {
    case TZ_DATE_YEAR:
      return numVal(range(kFirstYear, kLastYear, Origin::Trusted));
    case TZ_DATE_MONTH:
      return numVal(range(0, 11));
    case TZ_DATE_DATE:
      return numVal(range(1, 31));
    case TZ_DATE_DAY:
      return numVal(range(0, 6));
    case TZ_DATE_HOURS:
      return numVal(range(0, 23));
    case TZ_DATE_MINUTES:
    case TZ_DATE_SECONDS:
      return numVal(range(0, 59));
    case TZ_DATE_MS:
      return numVal(range(0, 999));
    default:
      return numVal(range(-1440, 1440));
    }
  }
  case Builtin::DecimalOf: {
    const Type *from =
        call->args.empty() || !call->args[0]->type ? nullptr : call->args[0]->type->nonOptional();
    Num x = arg(0).num;
    if (from && from->isDecimal() && !x.empty()) {
      return numVal(x);
    }
    if (from && from->isInteger() && !x.empty()) {
      bool exact = std::fabs(x.lo) <= 9007199254740992.0 && std::fabs(x.hi) <= 9007199254740992.0;
      return numVal(exact ? x : widenedDecimal(x));
    }
    if (from && from->isFloat() && !x.empty()) {
      return numVal(widenedDecimal(x));
    }
    if (auto *text =
            dyn_cast<StringLitExpr>(stripCasts(call->args.empty() ? nullptr : call->args[0]))) {
      std::string utf8(text->value.begin(), text->value.end());
      std::optional<DecimalLiteral> lit = readDecimalLiteral(utf8);
      if (lit && lit->isZero()) {
        return numVal(exactly(0));
      }
      if (lit) {
        std::string digits = lit->digits + "e" + std::to_string(lit->exponent);
        return numVal(widenedDecimal(exactly(std::strtod(digits.c_str(), nullptr))));
      }
    }
    return check_.overflowWarnings().trusted(type);
  }
  case Builtin::DecAbs: {
    Num x = receiver.num;
    if (x.empty()) {
      return check_.overflowWarnings().trusted(type);
    }
    Num r = make({x.lo >= 0   ? x.lo
                  : x.hi <= 0 ? -x.hi
                              : 0},
                 std::fabs(x.lo) > std::fabs(x.hi) ? Bound{-x.lo, x.loOrigin, x.loWhy} : x.high());
    r.away = x.away;
    return numVal(r);
  }
  case Builtin::DecSqrt: {
    Num x = receiver.num.empty() ? check_.overflowWarnings().trusted(type).num : receiver.num;
    if (!check_.overflowWarnings().checkDecimalSqrt(call, receiverExpr, x)) {
      return check_.overflowWarnings().trusted(type);
    }
    Num r = widenedDecimal(range(std::sqrt(std::max(x.lo, 0.0)), std::sqrt(x.hi)), 1e-20);
    r.lo = std::max(r.lo, 0.0);
    return numVal(r);
  }
  case Builtin::DecDiv: {
    Num x = receiver.num.empty() ? check_.overflowWarnings().trusted(type).num : receiver.num;
    Num n = arg(0).num.empty() ? check_.overflowWarnings().trusted(type).num : arg(0).num;
    bool ok = check_.overflowWarnings().checkDivisor(
        call, BinaryOp::Div, call->args.empty() ? nullptr : call->args[0], x, n);
    check_.overflowWarnings().noteProof(call, ok);
    return check_.overflowWarnings().trusted(type);
  }
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
  if (call->builtin >= Builtin::RegExpNew && call->builtin <= Builtin::StrSplitRegex) {
    check_.callRanges().calledUserCode(st);
  }
  return check_.overflowWarnings().trusted(type);
}

Val BuiltinEvaluator::evalMath(const CallExpr *call, State &st) {
  std::vector<Num> args;
  for (const Expr *arg : call->args) {
    Val v = check_.evaluator().eval(arg, st);
    const Type *type = arg->type ? arg->type->nonOptional() : nullptr;
    if (type && type->isArray()) {
      v = check_.slots().elementsOf(arg, v, type);
    }
    args.push_back(v.num);
  }
  const Type *type = call->type;
  auto arg = [&](size_t i) { return i < args.size() ? args[i] : Num(); };
  Num x = arg(0);
  OverflowWarnings &warnings = check_.overflowWarnings();
  auto monotoneOn = [&](double (*f)(double), double lo, double hi, const Num &v) {
    if (v.empty()) {
      return Num();
    }
    double a = f(std::clamp(v.lo, lo, hi)), b = f(std::clamp(v.hi, lo, hi));
    if (std::isnan(a) || std::isnan(b)) {
      return range(-kInf, kInf);
    }

    return make({a, v.loOrigin, v.loWhy}, {b, v.hiOrigin, v.hiWhy});
  };
  auto monotone = [&](double (*f)(double), double lo, double hi) {
    return monotoneOn(f, lo, hi, x);
  };
  auto even = [&](double (*f)(double), const Num &v) {
    if (v.empty()) {
      return Num();
    }
    Bound big = std::fabs(v.lo) > std::fabs(v.hi) ? v.low() : v.high();
    double small = v.lo <= 0 && v.hi >= 0 ? 0 : std::min(std::fabs(v.lo), std::fabs(v.hi));

    return make({f(small)}, {f(std::fabs(big.v)), big.origin, big.why});
  };
  auto suspect = [](Num n) {
    if (n.loOrigin == Origin::Trusted) {
      n.setLow({std::clamp(n.lo, -1.0, 1.0)});
    }
    if (n.hiOrigin == Origin::Trusted) {
      n.setHigh({std::clamp(n.hi, -1.0, 1.0)});
    }

    return n;
  };
  auto finish = [&](const Num &exact, const Num &exceeding) {
    if (exact.empty() || fits(exact, type)) {
      return numVal(exact);
    }
    if (!check_.quiet_) {
      warnings.overflowCall(call, x, exceeding);
    }

    return numVal(warnings.fit(exact, type));
  };
  auto domain = [&](double lo, double hi, bool loOpen, bool hiOpen, llvm::StringRef what,
                    llvm::StringRef condition, llvm::StringRef extra = "") {
    return warnings.checkDomain(call, call->args.empty() ? nullptr : call->args[0], x, lo, hi,
                                loOpen, hiOpen, what, condition, extra);
  };
  constexpr double kHalfPi = 1.5707963267948968, kPi = 3.1415926535897936;
  const double tiny = std::numeric_limits<double>::denorm_min();
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
      return Val();
    }
    Num best = args[0];
    for (size_t i = 1; i < args.size(); ++i) {
      const Num &a = args[i];
      if (a.empty() || best.empty()) {
        best = Num();
        break;
      }
      best = isMin ? make(lower(best.low(), a.low()), lower(best.high(), a.high()))
                   : make(higher(best.low(), a.low()), higher(best.high(), a.high()));
    }
    return numVal(check_.overflowWarnings().fit(best, type));
  }
  case Builtin::MathClamp: {
    Num lo = arg(1), hi = arg(2);
    warnings.checkClamp(call, lo, hi);
    if (lo.empty() || hi.empty()) {
      return warnings.trusted(type);
    }
    if (x.empty()) {
      return numVal(make(lo.low(), hi.high()));
    }
    Bound low = x.lo >= lo.lo ? x.low() : lo.low();
    Bound high = x.hi <= hi.hi ? x.high() : hi.high();
    if (low.v > hi.hi) {
      low = hi.high();
    }
    if (high.v < lo.lo) {
      high = lo.low();
    }
    return numVal(make(low, high));
  }
  case Builtin::MathSqrt:
    domain(0, kDoubleMax, false, false, "not negative", "x >= 0");
    return numVal(monotone([](double v) { return std::sqrt(v); }, 0, kDoubleMax));
  case Builtin::MathCbrt:
    return numVal(monotone([](double v) { return std::cbrt(v); }, -kInf, kInf));
  case Builtin::MathLog:
    domain(0, kDoubleMax, true, false, "above 0", "x > 0");
    return numVal(monotone([](double v) { return std::log(v); }, tiny, kDoubleMax));
  case Builtin::MathLog2:
    domain(0, kDoubleMax, true, false, "above 0", "x > 0");
    return numVal(monotone([](double v) { return std::log2(v); }, tiny, kDoubleMax));
  case Builtin::MathLog10:
    domain(0, kDoubleMax, true, false, "above 0", "x > 0");
    return numVal(monotone([](double v) { return std::log10(v); }, tiny, kDoubleMax));
  case Builtin::MathLog1p:
    domain(-1, kDoubleMax, true, false, "above -1", "x > -1");
    return numVal(
        monotone([](double v) { return std::log1p(v); }, std::nextafter(-1.0, 0.0), kDoubleMax));
  case Builtin::MathExp: {
    auto f = [](double v) { return std::exp(v); };
    return finish(monotone(f, -kInf, kInf), monotoneOn(f, -kInf, kInf, suspect(x)));
  }
  case Builtin::MathExpm1: {
    auto f = [](double v) { return std::expm1(v); };
    return finish(monotone(f, -kInf, kInf), monotoneOn(f, -kInf, kInf, suspect(x)));
  }
  case Builtin::MathSinh: {
    auto f = [](double v) { return std::sinh(v); };
    return finish(monotone(f, -kInf, kInf), monotoneOn(f, -kInf, kInf, suspect(x)));
  }
  case Builtin::MathCosh: {
    auto f = [](double v) { return std::cosh(v); };
    return finish(even(f, x), even(f, suspect(x)));
  }
  case Builtin::MathHypot: {
    if (args.empty()) {
      return numVal(exactly(0));
    }
    auto top = [&](bool suspected) {
      Bound big{0};
      for (const Num &a : args) {
        if (a.empty()) {
          return Num();
        }
        Num v = suspected ? suspect(a) : a;
        for (const Bound &b : {v.low(), v.high()}) {
          if (std::fabs(b.v) > big.v) {
            big = {std::fabs(b.v), b.origin, b.why};
          }
        }
      }
      big.v *= std::sqrt(double(args.size()));
      return make({0}, big);
    };
    return finish(top(false), top(true));
  }
  case Builtin::MathFround:
    domain(-kFloatMax, kFloatMax, false, false, "within ±3.4e38, the range of a float32",
           "x >= -3.4e38 && x <= 3.4e38");
    return numVal(x);
  case Builtin::MathSin:
  case Builtin::MathCos:
  case Builtin::MathTanh:
    return numVal(range(-1, 1));
  case Builtin::MathTan:
    return numVal(range(-1e17, 1e17));
  case Builtin::MathAtan:
    return numVal(range(-kHalfPi, kHalfPi));
  case Builtin::MathAtan2:
    return numVal(range(-kPi, kPi));
  case Builtin::MathAsin:
    domain(-1, 1, false, false, "from -1 to 1", "x >= -1 && x <= 1",
           ", or use 'Math.clamp(x, -1, 1)'");
    return numVal(range(-kHalfPi, kHalfPi));
  case Builtin::MathAcos:
    domain(-1, 1, false, false, "from -1 to 1", "x >= -1 && x <= 1",
           ", or use 'Math.clamp(x, -1, 1)'");
    return numVal(range(0, kPi));
  case Builtin::MathAsinh:
    return numVal(monotone([](double v) { return std::asinh(v); }, -kInf, kInf));
  case Builtin::MathAcosh:
    domain(1, kDoubleMax, false, false, "at least 1", "x >= 1");
    return numVal(monotone([](double v) { return std::acosh(v); }, 1, kDoubleMax));
  case Builtin::MathAtanh:
    domain(-1, 1, true, true, "between -1 and 1", "x > -1 && x < 1");
    return numVal(monotone([](double v) { return std::atanh(v); }, std::nextafter(-1.0, 0.0),
                           std::nextafter(1.0, 0.0)));
  case Builtin::MathRandom:
    return numVal(range(0, 0.9999999999999999));
  case Builtin::MathClz32:
    return numVal(range(0, 32));
  case Builtin::MathImul:
    return numVal(range(kInt32Min, kInt32Max));
  default:
    return check_.overflowWarnings().trusted(type);
  }
}

}
