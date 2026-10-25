#include "compiler/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MathExtras.h"
#include <limits>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

std::string rangeText(int64_t lo, int64_t hi) {
  return std::to_string(lo) + " to " + std::to_string(hi);
}

// What JavaScript stores of the integer v in an element of `kind`.
int64_t wrapped(Bytes kind, int64_t v) {
  switch (kind) {
  case Bytes::Int8:
    return int8_t(uint8_t(v));
  case Bytes::Int16:
    return int16_t(uint16_t(v));
  case Bytes::Uint16:
    return uint16_t(v);
  case Bytes::Uint32:
    return uint32_t(v);
  case Bytes::Int32:
    return int32_t(uint32_t(v));
  default:
    return uint8_t(v);
  }
}

Range exact(int64_t v) { return {v, v}; }

bool known(const Range &r) { return r.lo && r.hi; }

Range unite(const Range &a, const Range &b) {
  Range r;
  if (a.lo && b.lo) {
    r.lo = std::min(*a.lo, *b.lo);
  }
  if (a.hi && b.hi) {
    r.hi = std::max(*a.hi, *b.hi);
  }

  return r;
}

// The smallest 2^n - 1 at or above v >= 0.
int64_t allOnes(int64_t v) {
  int64_t m = 0;
  while (m < v) {
    m = m * 2 + 1;
  }

  return m;
}

int64_t floorShift(int64_t v, int64_t n) {
  return v >= 0 ? v >> n : -((-v + (int64_t(1) << n) - 1) >> n);
}

} // namespace

ByteProofs::ByteProofs(Checker &checker) : checker_(checker) {}

ValueRange ByteProofs::valueRange(const Expr *e) const {
  // Of `T | undefined`, what it holds when it holds a value: `u[i] ?? 0`.
  if (!e->type || !e->type->nonOptional()->isInteger()) {
    return {};
  }
  if (std::optional<int64_t> c = checker_.constantFolder().intConstant(e)) {
    return exact(*c);
  }
  bool isInt = e->type->nonOptional()->isInt();
  switch (e->kind()) {
  case Expr::Kind::Cast: {
    auto *conv = cast<CastExpr>(e);
    if (!conv->isImplicit() || !conv->operand->type || !conv->operand->type->isInteger()) {
      return {};
    }
    Range r = valueRange(conv->operand);
    // A long cut to its low 32 bits keeps its value if it fits in an int.
    if (isInt && conv->operand->type->kind() == Type::Kind::Long &&
        !(known(r) && *r.lo >= kIntMin && *r.hi <= kIntMax)) {
      return {};
    }
    return r;
  }
  case Expr::Kind::Name: {
    const VarSymbol *var = cast<NameExpr>(e)->var;
    if (!var || !var->type->isInt()) {
      return {};
    }
    // The bounds the facts give it: `x < 256`, `0 <= x`, `i < u.length` of
    // a u whose length is known.
    Range r;
    Term self = Term::of(var);
    if (std::optional<int64_t> above = checker_.flow_.facts.bound(self, Term::zero())) {
      r.hi = std::min(*above - 1, kIntMax); // x < above
    }
    if (std::optional<int64_t> below = checker_.flow_.facts.bound(Term::zero(), self)) {
      r.lo = std::max(1 - *below, kIntMin); // 0 < x + below
    }
    return r;
  }
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(e);
    if (member->name == "length" || member->name == "byteLength") {
      return {0, kMaxLength};
    }
    return {};
  }
  case Expr::Kind::Index: {
    // An element holds what its kind holds.
    auto *index = cast<IndexExpr>(e);
    int64_t lo, hi;
    if (index->object->type && index->object->type->elementRange(lo, hi)) {
      return {lo, hi};
    }
    return {};
  }
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(e);
    int64_t lo, hi;
    switch (call->builtin) {
    case Builtin::StrCharCodeAt:
      return {0, 0xFFFF};
    case Builtin::StrCodePointAt:
      return {0, 0x10FFFF};
    case Builtin::BytesAt:
      if (cast<MemberExpr>(call->callee)->object->type->elementRange(lo, hi)) {
        return {lo, hi};
      }
      return {};
    case Builtin::DataViewGet:
    case Builtin::BufferRead:
      if (Type::bytesType(Bytes(cast<IntLitExpr>(call->args[0])->value))->elementRange(lo, hi)) {
        return {lo, hi};
      }
      return {};
    case Builtin::MathMin:
    case Builtin::MathMax: {
      if (call->hasSpread() || call->args.empty()) {
        return {};
      }
      bool min = call->builtin == Builtin::MathMin;
      Range r = valueRange(call->args[0]);
      for (size_t i = 1; i < call->args.size(); ++i) {
        Range next = valueRange(call->args[i]);
        Range both;
        // min: below each upper bound; above the lowest of the lower ones.
        if (min) {
          if (r.hi || next.hi) {
            both.hi = r.hi && next.hi ? std::min(*r.hi, *next.hi) : r.hi ? *r.hi : *next.hi;
          }
          if (r.lo && next.lo) {
            both.lo = std::min(*r.lo, *next.lo);
          }
        } else {
          if (r.lo || next.lo) {
            both.lo = r.lo && next.lo ? std::max(*r.lo, *next.lo) : r.lo ? *r.lo : *next.lo;
          }
          if (r.hi && next.hi) {
            both.hi = std::max(*r.hi, *next.hi);
          }
        }
        r = both;
      }
      return r;
    }
    default:
      return {};
    }
  }
  case Expr::Kind::Unary: {
    auto *unary = cast<UnaryExpr>(e);
    Range r = valueRange(unary->operand);
    if (unary->op == UnaryOp::Plus) {
      return r;
    }
    if (!known(r)) {
      return {};
    }
    if (unary->op == UnaryOp::Neg) {
      return operate(BinaryOp::Sub, exact(0), r, isInt);
    }
    if (unary->op == UnaryOp::BitNot) {
      return {-*r.hi - 1, -*r.lo - 1};
    }
    return {};
  }
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(e);
    if (bin->op == BinaryOp::Nullish) {
      return unite(valueRange(bin->lhs), valueRange(bin->rhs));
    }
    // An int's `>>>` is a long, of a 32-bit shift.
    bool shift = bin->op == BinaryOp::Shl || bin->op == BinaryOp::Shr || bin->op == BinaryOp::UShr;
    return operate(bin->op, valueRange(bin->lhs), valueRange(bin->rhs),
                   shift ? bin->lhs->type->isInt() : isInt);
  }
  case Expr::Kind::Conditional: {
    auto *cond = cast<ConditionalExpr>(e);
    return unite(valueRange(cond->thenExpr), valueRange(cond->elseExpr));
  }
  case Expr::Kind::Sequence:
    return valueRange(cast<SequenceExpr>(e)->exprs.back());
  default:
    return {};
  }
}

bool ByteProofs::provesFits(const Expr *expr, int64_t lo, int64_t hi) const {
  // An int widened to a long keeps its value.
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit() || !conv->operand->type || !conv->operand->type->isInt() ||
        !conv->type->isInteger()) {
      break;
    }
    expr = conv->operand;
  }
  if (auto *cond = dyn_cast<ConditionalExpr>(expr)) {
    return provesFits(cond->thenExpr, lo, hi) && provesFits(cond->elseExpr, lo, hi);
  }
  bool lower = false, upper = false;
  // `x` or `x + k` with facts about x: `if (x >= 0 && x <= 255)`.
  if (std::optional<Linear> l = checker_.indexProofs().linear(expr)) {
    if (l->term.isZero()) {
      lower = l->offset >= lo;
      upper = l->offset <= hi;
    } else {
      lower = checker_.flow_.facts.proves(Term::zero(), l->term, l->offset - lo + 1);
      upper = checker_.flow_.facts.proves(l->term, Term::zero(), hi - l->offset + 1);
    }
  }
  Range r = valueRange(expr);
  lower |= r.lo && *r.lo >= lo;
  upper |= r.hi && *r.hi <= hi;

  return lower && upper;
}

void ByteProofs::checkFits(const Expr *value, const Type *target, llvm::StringRef what) {
  int64_t lo, hi;
  // A floating element holds any number, losing precision only; a value
  // that is no integer was reported where it was converted.
  if (!value->type->isInteger() || !target->elementRange(lo, hi)) {
    return;
  }
  Bytes kind = target->bytes();
  // A clamped element takes any int (that is what it is for), and one of an
  // Int32Array is an int.
  if (kind == Bytes::Uint8Clamped || kind == Bytes::Int32 || provesFits(value, lo, hi)) {
    return;
  }
  std::string text = valueText(value);
  if (std::optional<int64_t> c = checker_.constantFolder().intConstant(value)) {
    checker_.diags_.error(value->loc(), std::to_string(*c) + " does not fit in " + what.str() +
                                            " (" + rangeText(lo, hi) +
                                            "): JavaScript would store " +
                                            std::to_string(wrapped(kind, *c)) +
                                            "; write a value that fits, or cut it explicitly: '" +
                                            wrapHint(kind, std::to_string(*c)) + "'");
    return;
  }
  checker_.diags_.error(
      value->loc(),
      "cannot prove that the value fits in " + what.str() + " (" + rangeText(lo, hi) +
          "), and JavaScript would cut it without a word; check it first, e.g. 'if (" + text +
          " >= " + std::to_string(lo) + " && " + text + " <= " + std::to_string(hi) +
          ")', or cut it explicitly: '" + wrapHint(kind, text) + "'");
}

bool ByteProofs::provesWithin(const Expr *object, const Expr *offset, int64_t size) const {
  std::optional<Linear> at =
      offset ? checker_.indexProofs().linear(offset) : Linear{Term::zero(), 0};
  std::optional<Linear> length = checker_.indexProofs().lengthOf(object);
  if (!at || !length) {
    return false;
  }

  // 0 <= at, and at + size <= length, that is at < length - size + 1.
  return checker_.flow_.facts.proves(Term::zero(), at->term, at->offset + 1) &&
         checker_.flow_.facts.proves(at->term, length->term,
                                     length->offset - size + 1 - at->offset);
}

void ByteProofs::addByteCallbackFacts(const FunctionDecl *fn) {
  auto it = checker_.byteCallbacks_.find(fn);
  if (it == checker_.byteCallbacks_.end() || it->second.param >= fn->params.size()) {
    return;
  }
  const Checker::ByteCallback &info = it->second;
  const VarSymbol *var = fn->params[info.param].sym;
  if (!var || !var->type->isInt() || (info.lo <= kIntMin && info.hi >= kIntMax)) {
    return;
  }
  // lo <= x and x <= hi, as the facts write them.
  checker_.flow_.facts.add(Term::zero(), Term::of(var), 1 - info.lo);
  checker_.flow_.facts.add(Term::of(var), Term::zero(), info.hi + 1);
}

void ByteProofs::checkByteCallbackResult(const FunctionDecl *fn, const Expr *value) {
  auto it = checker_.byteCallbacks_.find(fn);
  if (it == checker_.byteCallbacks_.end() || !it->second.target) {
    return;
  }
  checkFits(value, it->second.target, "an element of '" + it->second.target->name().str() + "'");
}

} // namespace tz
