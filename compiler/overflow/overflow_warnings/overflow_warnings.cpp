#include "compiler/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/overflow/program_scan/program_scan.h"
#include "compiler/overflow/slots/slots.h"
#include "compiler/overflow/call_ranges/call_ranges.h"
#include "compiler/overflow/executor/executor.h"
#include "compiler/overflow/loop_ranges/loop_ranges.h"
#include "compiler/overflow/narrowing/narrowing.h"
#include "compiler/overflow/evaluator/evaluator.h"
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

// The text of an expression as written, as far as it can be rebuilt.
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

// A value named in a message: 'x', 'p.count', or a description.
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
  if (std::fabs(v) < 1e21) {
    std::snprintf(text, sizeof text, "%.0f", v);
  } else {
    std::snprintf(text, sizeof text, "%g", v);
  }

  return text;
}

} // namespace

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

// A value of `type` about which nothing is known, but which is the
// program's own.
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
  // Before the last pass of a loop: as it is, so that a sum stays a sum.
  if (check_.quiet_ && isIntegerType(type)) {
    return n;
  }
  Limits l = limitsOf(type);
  if (n.lo >= l.lo && n.hi <= l.hi) {
    return n;
  }
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
      std::string init = check_.programScan().isConstantTree(decl->init) ? exprText(decl->init) : "...";
      return "declare it as 'let " + accum->name.str() + ": long = " + init + "', or as a double";
    }
    if (decl && decl->typeAnn) {
      return "declare '" + accum->name.str() + "' as 'long' or 'double'";
    }
  }
  // A field that adds up: `this.total += x`.
  const Expr *target = nullptr;
  if (auto *assign = dyn_cast<AssignExpr>(site); assign && assign->op) {
    target = assign->target;
  } else if (auto *update = dyn_cast<UpdateExpr>(site)) {
    target = update->target;
  }
  if (auto *member = dyn_cast_or_null<MemberExpr>(target)) {
    return "make the field '" + member->name.str() + "' a 'long' or a 'double'";
  }
  auto init = check_.inits_.find(site);
  if (init != check_.inits_.end() && init->second.first && !init->second.first->typeAnn &&
      init->second.first->sym) {
    const VarDeclarator *decl = init->second.first;
    return std::string("compute it as a long: '") + (init->second.second ? "const " : "let ") +
           decl->name.str() + ": long = " + exprText(site) +
           "', check the value before, or write '| 0' if wrapping is meant";
  }

  return "check the value before, compute it as a 'long' or a 'double', or write '| 0' if "
         "wrapping is meant";
}

void OverflowWarnings::warn(const Expr *site, const Num &l, const Num &r, const Expr *lhs,
                         const Expr *rhs, const Num &exceeding, const VarSymbol *accum) {
  if (!check_.reporting_ || check_.quiet_) {
    return;
  }
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
  } else {
    why = "it may reach " + numberText(top ? exceeding.hi : exceeding.lo);
  }
  check_.warnings_.push_back(
      {loc, "'" + exprText(site) + "' may overflow int: " + why + "; " + fixText(site, accum)});
}

} // namespace tz
