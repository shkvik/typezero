#include "compiler/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/facts/facts.h"
#include "compiler/overflow/call_ranges/call_ranges.h"
#include "compiler/overflow/evaluator/evaluator.h"
#include "compiler/overflow/executor/executor.h"
#include "compiler/overflow/loop_ranges/loop_ranges.h"
#include "compiler/overflow/narrowing/narrowing.h"
#include "compiler/overflow/overflow_helpers/overflow_helpers.h"
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
#include <cstdlib>
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

std::string numberText(double v);

std::string exprText(const Expr *expr) {
  expr = stripCasts(expr);
  if (!expr) {
    return "...";
  }
  switch (expr->kind()) {
  case Expr::Kind::Name:
    return cast<NameExpr>(expr)->name.str();
  case Expr::Kind::IntLit: {
    auto *lit = cast<IntLitExpr>(expr);
    return lit->text.empty() ? std::to_string(lit->value) : lit->text.str();
  }
  case Expr::Kind::FloatLit: {
    auto *lit = cast<FloatLitExpr>(expr);
    return lit->text.empty() ? numberText(lit->value) : lit->text.str();
  }
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(expr);
    return exprText(member->object) + "." + member->name.str();
  }
  case Expr::Kind::Binary:
    if (!cast<BinaryExpr>(expr)->source.empty()) {
      return cast<BinaryExpr>(expr)->source.str();
    }
    return exprText(cast<BinaryExpr>(expr)->lhs) + " " + opSpelling(cast<BinaryExpr>(expr)->op) +
           " " + exprText(cast<BinaryExpr>(expr)->rhs);
  case Expr::Kind::Assign: {
    auto *assign = cast<AssignExpr>(expr);
    if (!assign->source.empty()) {
      return assign->source.str();
    }
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

std::string subject(const Expr *expr) {
  expr = stripCasts(expr);
  if (!expr) {
    return "the value";
  }
  if (isa<NameExpr>(expr) || isa<MemberExpr>(expr)) {
    return "'" + exprText(expr) + "'";
  }
  if (auto *index = dyn_cast<IndexExpr>(expr)) {
    return "the element of '" + exprText(index->object) + "'";
  }
  if (auto *call = dyn_cast<CallExpr>(expr)) {
    return "the result of '" + exprText(call) + "'";
  }

  return "'" + exprText(expr) + "'";
}

std::string numberText(double v) {
  if (std::fabs(v) == kInf) {
    return v > 0 ? "any size" : "any negative size";
  }
  char text[64];
  if (std::fabs(v) < 1e21 && v == std::trunc(v)) {
    std::snprintf(text, sizeof text, "%.0f", v);
    return text;
  }
  for (int digits = 1; digits <= 17; ++digits) {
    std::snprintf(text, sizeof text, "%.*g", digits, v);
    if (std::strtod(text, nullptr) == v) {
      break;
    }
  }

  return text;
}

std::string replaced(std::string text, llvm::StringRef from, llvm::StringRef to) {
  for (size_t at = text.find(from.str()); at != std::string::npos;
       at = text.find(from.str(), at + to.size())) {
    text.replace(at, from.size(), to.str());
  }

  return text;
}

}

OverflowWarnings::OverflowWarnings(OverflowCheck &check) : check_(check) {}

const Reason *OverflowWarnings::reason(Origin origin, llvm::SMLoc loc, std::string what,
                                       bool unbounded) {
  auto key = std::make_pair(loc.getPointer(),
                            what + (unbounded ? "!" : "") + char('0' + static_cast<int>(origin)));
  auto found = check_.reasonIndex_.find(key);
  if (found != check_.reasonIndex_.end()) {
    return found->second;
  }
  Reason &r = check_.reasons_.emplace_back();
  r.origin = origin;
  r.loc = loc;
  r.what = std::move(what);
  r.unbounded = unbounded;
  check_.reasonIndex_[key] = &r;

  return &r;
}

Num OverflowWarnings::input(const Type *type, llvm::SMLoc loc, const char *what) {
  Limits l = limitsOf(type);

  return range(l.lo, l.hi, Origin::Input, reason(Origin::Input, loc, what));
}

Val OverflowWarnings::trusted(const Type *type) const {
  Val v;
  if (!type) {
    return v;
  }
  if (isNumberType(type)) {
    Limits l = limitsOf(type);
    v.num = range(l.lo, l.hi, Origin::Trusted);
  }
  if (hasLength(type)) {
    v.len = someLength();
  }

  return v;
}

Num OverflowWarnings::fit(Num n, const Type *type) const {
  if (n.empty() || !isNumberType(type)) {
    return n;
  }
  if (check_.quiet_ && (isIntegerType(type) || isFloatType(type))) {
    return n;
  }
  Limits l = limitsOf(type);
  if (n.lo >= l.lo && n.hi <= l.hi) {
    return n;
  }
  if (isIntegerType(type)) {
    Bound w = n.hi > l.hi && n.lo < l.lo ? joint(0, n.low(), n.high())
              : n.hi > l.hi              ? n.high()
                                         : n.low();
    return range(l.lo, l.hi, w.origin, w.why);
  }
  if (isFloatType(type)) {
    if (n.lo < l.lo) {
      n.setLow({l.lo, Origin::Trusted});
    }
    if (n.hi > l.hi) {
      n.setHigh({l.hi, Origin::Trusted});
    }
    return n;
  }

  return narrowTo(n, l.lo, l.hi);
}

Val OverflowWarnings::fitVal(Val v, const Type *type) const {
  if (!type) {
    return v;
  }
  if (isNumberType(type)) {
    v.num = fit(v.num, type);
  }
  if (hasLength(type) && !v.len.empty()) {
    v.len = narrowTo(v.len, 0, kLengthMax);
  }

  return v;
}

std::string OverflowWarnings::line(llvm::SMLoc loc, llvm::SMLoc from) const {
  llvm::SourceMgr &sources = check_.diags_.sourceMgr();
  unsigned id = sources.FindBufferContainingLoc(loc);
  if (!id) {
    return "an unknown line";
  }
  std::string text = "line " + std::to_string(sources.getLineAndColumn(loc, id).first);
  if (id != sources.FindBufferContainingLoc(from)) {
    text += " of " +
            llvm::sys::path::filename(sources.getMemoryBuffer(id)->getBufferIdentifier()).str();
  }

  return text;
}

std::string OverflowWarnings::inputText(const Reason *why, llvm::SMLoc from) const {
  if (!why) {
    return "outside the program";
  }
  if (!why->calls.empty()) {
    std::string text = inputText(why->via, from);
    if (why->calls.size() == 1) {
      return text + ", passed by the call on " + line(why->calls[0], from);
    }
    text += ", passed by the calls on lines ";
    for (size_t i = 0; i < why->calls.size() && i < 4; ++i) {
      if (i) {
        text += i + 1 == why->calls.size() ? " and " : ", ";
      }
      std::string l = line(why->calls[i], from);
      text += llvm::StringRef(l).drop_front(5).str();
    }
    if (why->calls.size() > 4) {
      text += " and others";
    }
    return text;
  }

  return why->what + " on " + line(why->loc, from);
}

std::string OverflowWarnings::fixText(const Expr *site, const VarSymbol *accum) const {
  if (accum) {
    auto info = check_.vars_.find(accum);
    const VarDeclarator *decl = info != check_.vars_.end() ? info->second.decl : nullptr;
    if (decl && !decl->typeAnn && decl->init && !info->second.isConst) {
      std::string init =
          check_.programScan().isConstantTree(decl->init) ? exprText(decl->init) : "...";
      return "check the values before, or count in a decimal: 'let " + accum->name.str() +
             ": decimal = " + init + "' and add 'x.toDecimal()'";
    }
    if (decl && decl->typeAnn) {
      return "check the values before, or count '" + accum->name.str() + "' in a 'decimal'";
    }
  }
  const Expr *target = nullptr;
  if (auto *assign = dyn_cast<AssignExpr>(site); assign && assign->op) {
    target = assign->target;
  } else if (auto *update = dyn_cast<UpdateExpr>(site)) {
    target = update->target;
  }
  if (auto *member = dyn_cast_or_null<MemberExpr>(target)) {
    return "check the value before, or make the field '" + member->name.str() + "' a 'decimal'";
  }
  if (isFloatType(site->type)) {
    return "check the values before, or compute it as a 'decimal'";
  }

  return "check the values before, or compute it in a wider type or as a 'decimal'";
}

void OverflowWarnings::noteProof(const Expr *site, bool proven) {
  if (check_.reporting_ && !check_.quiet_) {
    auto [at, fresh] = check_.proofs_.try_emplace(site, true);
    at->second = at->second && proven;
  }
}

bool OverflowWarnings::checkDivisor(const Expr *site, BinaryOp op, const Expr *divisor,
                                    const Num &l, const Num &r) {
  bool zero = op == BinaryOp::Pow ? l.lo <= 0 && l.hi >= 0 && awayOf(l) == 0 && r.lo < 0
                                  : r.lo <= 0 && r.hi >= 0 && awayOf(r) == 0;
  if (!zero || !check_.reporting_ || check_.quiet_) {
    return !zero;
  }
  llvm::SMLoc loc = divisor ? divisor->loc() : site->loc();
  std::string text = divisor ? exprText(divisor) : "x";
  if (op != BinaryOp::Pow && r.lo == 0 && r.hi == 0 && divisor &&
      check_.programScan().isConstantTree(divisor)) {
    if (isFloatType(site->type)) {
      check_.warnings_.push_back({loc, "division by zero"});
    }
    return false;
  }
  if (op == BinaryOp::Pow) {
    check_.warnings_.push_back(
        {site->loc(), "cannot prove that '" + exprText(site) +
                          "' does not raise 0 to a negative power, which divides by zero; "
                          "check the base first, e.g. 'if (" +
                          text + " != 0)'"});
    return false;
  }
  std::string why;
  if (r.lo == 0 && r.hi == 0) {
    why = "it is 0";
  } else if (r.loOrigin == Origin::Input || r.hiOrigin == Origin::Input) {
    why = subject(divisor) + " comes from " +
          inputText(r.loOrigin == Origin::Input ? r.loWhy : r.hiWhy, loc);
  } else {
    why = "it may be 0";
  }
  check_.warnings_.push_back({loc, "cannot prove that the divisor '" + text + "' is not 0: " + why +
                                       "; check it first, e.g. 'if (" + text + " != 0)'"});

  return false;
}

bool OverflowWarnings::checkPowerBase(const Expr *site, const Expr *base, const Num &a) {
  if (a.empty() || a.lo >= 0 || a.loOrigin == Origin::Trusted) {
    return true;
  }
  if (!check_.reporting_ || check_.quiet_) {
    return false;
  }
  std::string text = exprText(base);
  std::string why = a.loOrigin == Origin::Input
                        ? subject(base) + " comes from " + inputText(a.loWhy, site->loc())
                        : "it may be " + numberText(a.lo);
  check_.warnings_.push_back(
      {site->loc(), "cannot prove that the base of '" + exprText(site) +
                        "' is not negative, which a fractional power needs (JavaScript gives "
                        "NaN): " +
                        why + "; check it first, e.g. 'if (" + text +
                        " >= 0)', or raise it to an integer power"});

  return false;
}

bool OverflowWarnings::checkDomain(const CallExpr *call, const Expr *value, const Num &x, double lo,
                                   double hi, bool loOpen, bool hiOpen, llvm::StringRef what,
                                   llvm::StringRef condition, llvm::StringRef extra) {
  if (x.empty()) {
    return true;
  }
  bool low = (loOpen ? x.lo <= lo : x.lo < lo) && x.loOrigin != Origin::Trusted;
  bool high = (hiOpen ? x.hi >= hi : x.hi > hi) && x.hiOrigin != Origin::Trusted;
  if (!low && !high) {
    return true;
  }
  if (!check_.reporting_ || check_.quiet_) {
    return false;
  }
  Bound culprit = low ? x.low() : x.high();
  llvm::SMLoc loc = value ? value->loc() : call->loc();
  std::string why;
  if (culprit.origin == Origin::Input) {
    why = subject(value) + " comes from " + inputText(culprit.why, loc);
  } else if (culprit.origin == Origin::Loop) {
    why = subject(value) + " grows with every pass of a loop";
  } else {
    why = "it may be " + numberText(culprit.v);
  }
  std::string text = exprText(value);
  check_.warnings_.push_back(
      {loc, "cannot prove that the argument of '" + exprText(call->callee) + "' is " + what.str() +
                ": " + why + "; check it first, e.g. 'if (" + replaced(condition.str(), "x", text) +
                ")'" + replaced(extra.str(), "x", text)});

  return false;
}

bool OverflowWarnings::checkClamp(const CallExpr *call, const Num &lo, const Num &hi) {
  if (lo.empty() || hi.empty() || lo.hi <= hi.lo) {
    return true;
  }
  if (!check_.reporting_ || check_.quiet_ || call->args.size() < 3) {
    return false;
  }
  std::string low = exprText(call->args[1]), high = exprText(call->args[2]);
  check_.warnings_.push_back(
      {call->args[1]->loc(), "cannot prove that the low bound of 'Math.clamp' is not above the "
                             "high one: '" +
                                 low + "' may be " + numberText(lo.hi) + " and '" + high + "' " +
                                 numberText(hi.lo) + "; check it first, e.g. 'if (" + low +
                                 " <= " + high + ")'"});

  return false;
}

void OverflowWarnings::overflowCall(const CallExpr *call, const Num &x, const Num &exceeding) {
  if (exceeding.empty() || fits(exceeding, call->type)) {
    return;
  }
  warn(call, x, Num(), call->args.empty() ? nullptr : call->args[0], nullptr, exceeding, nullptr,
       call->type);
}

void OverflowWarnings::checkShiftCount(const Expr *count, const Num &n, unsigned width) {
  if (n.empty() || (n.lo >= 0 && n.hi < width) || !check_.reporting_ || check_.quiet_) {
    return;
  }
  std::string text = exprText(count);
  std::string why;
  Bound culprit = n.hi >= width ? n.high() : n.low();
  if (culprit.origin == Origin::Input) {
    why = subject(count) + " comes from " + inputText(culprit.why, count->loc());
  } else {
    why = "it may be " + numberText(culprit.v);
  }
  check_.warnings_.push_back(
      {count->loc(), "cannot prove that the shift count '" + text + "' is from 0 to " +
                         std::to_string(width - 1) + ", the bits of the type: " + why +
                         "; check it first, e.g. 'if (" + text + " >= 0 && " + text + " < " +
                         std::to_string(width) + ")'"});
}

void OverflowWarnings::checkDate(const Num &time, double limit, llvm::ArrayRef<DatePart> parts) {
  if (time.empty() || (time.lo >= -limit && time.hi <= limit)) {
    return;
  }
  bool low = time.lo < -limit && time.loOrigin != Origin::Trusted;
  bool high = time.hi > limit && time.hiOrigin != Origin::Trusted;
  if ((!low && !high) || !check_.reporting_ || check_.quiet_) {
    return;
  }
  auto above = [&](const DatePart &part) {
    return high && part.num.hi > part.hi && part.num.hiOrigin != Origin::Trusted;
  };
  auto below = [&](const DatePart &part) {
    return low && part.num.lo < part.lo && part.num.loOrigin != Origin::Trusted;
  };
  std::vector<const DatePart *> blamed;
  for (const DatePart &part : parts) {
    if ((above(part) && dangerous(part.num.hiOrigin)) ||
        (below(part) && dangerous(part.num.loOrigin))) {
      blamed.push_back(&part);
    }
  }
  for (const DatePart &part : parts) {
    if (blamed.empty() && (above(part) || below(part))) {
      blamed.push_back(&part);
    }
  }
  for (const DatePart *part : blamed) {
    Bound culprit = above(*part) ? part->num.high() : part->num.low();
    llvm::SMLoc loc = part->expr->loc();
    std::string why = subject(part->expr);
    if (culprit.origin == Origin::Input) {
      why += " comes from " + inputText(culprit.why, loc);
    } else if (culprit.origin == Origin::Loop) {
      why += " grows with every pass of a loop";
    } else {
      why += " may be " + numberText(culprit.v);
    }
    std::string text = exprText(part->expr);
    check_.warnings_.push_back(
        {loc, "cannot prove that the time of the date fits in 'int' (about 292 million years "
              "around 1970): " +
                  why + "; check it first, e.g. 'if (" + text + " >= " + numberText(part->lo) +
                  " && " + text + " <= " + numberText(part->hi) + ")'"});
  }
}

bool OverflowWarnings::checkConversion(const CallExpr *call, const Expr *value, const Num &x,
                                       const Type *to) {
  bool toDouble = to->isFloat();
  constexpr double kExact = 9007199254740992.0;
  double lo = toDouble ? -kExact : limitsOf(to).lo, hi = toDouble ? kExact : limitsOf(to).hi;
  bool lowFits = toDouble ? x.lo >= lo : fitsLow(x.lo, to);
  bool highFits = toDouble ? x.hi <= hi : fitsHigh(x.hi, to);
  bool proven = lowFits && highFits;
  if (!toDouble) {
    noteProof(call, proven);
  }
  if (proven) {
    return true;
  }
  bool blameLow = !lowFits && x.loOrigin != Origin::Trusted;
  bool blameHigh = !highFits && x.hiOrigin != Origin::Trusted;
  if (!blameLow && !blameHigh) {
    return false;
  }
  if (!check_.reporting_ || check_.quiet_) {
    return false;
  }
  Bound culprit = blameHigh ? x.high() : x.low();
  llvm::SMLoc loc = call->loc();
  std::string what = subject(value);
  std::string why;
  if (culprit.origin == Origin::Input) {
    why = what + " comes from " + inputText(culprit.why, loc);
  } else if (culprit.origin == Origin::Loop) {
    why = what + " grows with every pass of a loop";
  } else {
    why = "it may be " + numberText(culprit.v);
  }
  std::string text = exprText(value);
  std::string low = toDouble ? numberText(lo) : std::to_string(to->intMin()),
              high = toDouble ? numberText(hi) : std::to_string(to->intMax());
  std::string message;
  if (toDouble) {
    message = "cannot prove that " + what +
              " is within ±9007199254740992 (2^53), where a double holds every integer "
              "exactly: " +
              why + "; check it first, e.g. 'if (" + text + " >= -9007199254740992 && " + text +
              " <= 9007199254740992)', or keep it exact with 'toDecimal()'";
  } else {
    message = "cannot prove that " + what + " fits in '" + to->name().str() + "' (" + low + " to " +
              high + "): " + why + "; check it first, e.g. 'if (" + text + " >= " + low + " && " +
              text + " <= " + high + ")'";
  }
  check_.warnings_.push_back({loc, message});

  return false;
}

void OverflowWarnings::warn(const Expr *site, const Num &l, const Num &r, const Expr *lhs,
                            const Expr *rhs, const Num &exceeding, const VarSymbol *accum,
                            const Type *type) {
  if (!check_.reporting_ || check_.quiet_) {
    return;
  }
  bool top = !fitsHigh(exceeding.hi, type), bottom = !fitsLow(exceeding.lo, type);
  Bound culprit = top && bottom ? joint(0, exceeding.low(), exceeding.high())
                  : top         ? exceeding.high()
                                : exceeding.low();
  const Expr *subjectExpr = nullptr;
  auto operands = accum ? std::array{std::pair{&r, rhs}, std::pair{&l, lhs}}
                        : std::array{std::pair{&l, lhs}, std::pair{&r, rhs}};
  for (int exact = 1; exact >= 0 && !subjectExpr; --exact) {
    for (auto [n, e] : operands) {
      if (!e || subjectExpr) {
        continue;
      }
      for (const Bound &b : {n->low(), n->high()}) {
        if (b.origin == culprit.origin && (!exact || b.why == culprit.why)) {
          subjectExpr = e;
        }
      }
    }
  }
  std::string why;
  llvm::SMLoc loc = site->loc();
  const Expr *divisor = nullptr;
  if (auto *bin = dyn_cast<BinaryExpr>(site); bin && bin->op == BinaryOp::Div) {
    divisor = bin->rhs;
  } else if (auto *assign = dyn_cast<AssignExpr>(site);
             assign && assign->op && *assign->op == BinaryOp::Div) {
    divisor = assign->value;
  }
  if (isFloatType(type) && divisor && r.lo < 1 && r.hi > -1) {
    std::string text = exprText(divisor);
    check_.warnings_.push_back(
        {loc, "'" + exprText(site) +
                  "' may overflow double: it may go beyond ±1.8e308, where a double becomes "
                  "Infinity, as the divisor " +
                  subject(divisor) + " may be close to 0; check it first, e.g. 'if (Math.abs(" +
                  text + ") >= 0.000001)'"});
    return;
  }
  if (culprit.origin == Origin::Input) {
    why = subject(subjectExpr) + " comes from " + inputText(culprit.why, loc);
  } else if (culprit.origin == Origin::Loop) {
    const Reason *loop = culprit.why;
    while (loop && !loop->calls.empty()) {
      loop = loop->via;
    }
    why = subject(subjectExpr) + " grows with every pass of the loop on " +
          (loop ? line(loop->loc, loc) : std::string("an unknown line")) +
          (loop && loop->unbounded ? ", which has no bound" : "");
    if (culprit.why && !culprit.why->calls.empty()) {
      why += ", passed by the call on " + line(culprit.why->calls[0], loc);
    }
  } else if (isFloatType(type)) {
    why = "it may go beyond ±1.8e308, where a double becomes Infinity";
  } else {
    why = "it may reach " + numberText(top ? exceeding.hi : exceeding.lo);
  }
  check_.warnings_.push_back({loc, "'" + exprText(site) + "' may overflow " +
                                       type->nonOptional()->name().str() + ": " + why + "; " +
                                       fixText(site, accum)});
}

}
