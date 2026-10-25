#include "compiler/overflow/builtin_evaluator/builtin_evaluator.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/facts/facts.h"
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

// The times of Date: 100 million days either side of 1970, in milliseconds.
constexpr double kMaxTime = 8.64e15;

} // namespace

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

Val BuiltinEvaluator::evalMath(const CallExpr *call, State &st) {
  std::vector<Num> args;
  for (const Expr *arg : call->args) {
    Val v = check_.evaluator().eval(arg, st);
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

Val BuiltinEvaluator::jsonVal(const Type *type, const Reason *why) {
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
void BuiltinEvaluator::jsonValues(const Type *type, const Reason *why,
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

} // namespace tz
