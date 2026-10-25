#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/library/bytes/byte_proofs/byte_proofs.h"
#include "llvm/ADT/STLExtras.h"
#include <limits>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr int64_t kIntMin = std::numeric_limits<int32_t>::min();

constexpr int64_t kIntMax = std::numeric_limits<int32_t>::max();

// Larger constants are not tracked, which keeps all offsets small.
constexpr uint64_t kMaxConstant = uint64_t(1) << 40;

// The variable `expr` reads, looking through the conversion Sema inserts for
// a narrowed `T | undefined` variable.
const VarSymbol *variableOf(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr);
      conv && conv->isImplicit() && conv->operand->type->isOptional()) {
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name ? name->var : nullptr;
}

} // namespace

IndexProofs::IndexProofs(Sema &sema) : sema_(sema) {}

std::optional<Linear> IndexProofs::linear(const Expr *expr) const {
  switch (expr->kind()) {
  case Expr::Kind::IntLit: {
    auto *lit = cast<IntLitExpr>(expr);
    if (!expr->type->isInteger() || lit->value > kMaxConstant) {
      return std::nullopt;
    }
    return Linear{Term::zero(), static_cast<int64_t>(lit->value)};
  }
  case Expr::Kind::Unary: {
    auto *unary = cast<UnaryExpr>(expr);
    auto *lit = dyn_cast<IntLitExpr>(unary->operand);
    if (unary->op != UnaryOp::Neg || !lit || !expr->type->isInteger() ||
        lit->value > kMaxConstant) {
      return std::nullopt;
    }
    return Linear{Term::zero(), -static_cast<int64_t>(lit->value)};
  }
  case Expr::Kind::Name: {
    const VarSymbol *var = cast<NameExpr>(expr)->var;
    if (var && var->type->isInt()) {
      return Linear{Term::of(var), 0};
    }
    return std::nullopt;
  }
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(expr);
    if (member->name == "length" && expr->type->isInt()) {
      return lengthOf(member->object);
    }
    // The bytes of an ArrayBuffer or a DataView, which index them.
    if (member->name == "byteLength" && expr->type->isInt() && member->object->type &&
        (member->object->type->bytes() == Type::Bytes::ArrayBuffer ||
         member->object->type->bytes() == Type::Bytes::DataView)) {
      return lengthOf(member->object);
    }
    return std::nullopt;
  }
  case Expr::Kind::Cast: {
    // int widened to long keeps its value.
    auto *conv = cast<CastExpr>(expr);
    if (conv->isImplicit() && conv->operand->type->isInt() && conv->type->isInteger()) {
      return linear(conv->operand);
    }
    return std::nullopt;
  }
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    if ((bin->op != BinaryOp::Add && bin->op != BinaryOp::Sub) || !expr->type->isInteger()) {
      return std::nullopt;
    }
    std::optional<Linear> l = linear(bin->lhs), r = linear(bin->rhs);
    if (!l || !r) {
      return std::nullopt;
    }
    Linear result;
    if (r->term.isZero()) {
      result = {l->term, bin->op == BinaryOp::Add ? l->offset + r->offset : l->offset - r->offset};
    } else if (bin->op == BinaryOp::Add && l->term.isZero()) {
      result = {r->term, l->offset + r->offset};
    } else {
      return std::nullopt;
    }
    if (result.offset > int64_t(kMaxConstant) || result.offset < -int64_t(kMaxConstant)) {
      return std::nullopt;
    }
    if (expr->type->isInt()) {
      // Computed in 32 bits: linear only if it provably does not wrap around.
      const Facts &facts = sema_.flow_.facts;
      if (result.term.isZero()) {
        if (result.offset < kIntMin || result.offset > kIntMax) {
          return std::nullopt;
        }
      } else if (!facts.proves(result.term, Term::zero(), kIntMax - result.offset + 1) ||
                 !facts.proves(Term::zero(), result.term, result.offset - kIntMin + 1)) {
        return std::nullopt;
      }
    }
    return result;
  }
  default:
    return std::nullopt;
  }
}

std::optional<Linear> IndexProofs::lengthOf(const Expr *object) const {
  if (auto *lit = dyn_cast<StringLitExpr>(object)) {
    return Linear{Term::zero(), static_cast<int64_t>(lit->value.size())};
  }
  if (auto *lit = dyn_cast<ArrayLitExpr>(object)) {
    if (llvm::any_of(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread; })) {
      return std::nullopt;
    }
    return Linear{Term::zero(), static_cast<int64_t>(lit->elements.size())};
  }
  if (const VarSymbol *var = variableOf(object)) {
    const Type *type = var->type->nonOptional();
    // The length of a typed array never changes (nor the bytes of an
    // ArrayBuffer or a DataView), so facts about it outlive calls.
    if (type->isString() || type->isArray() || type->isByteView() ||
        type->bytes() == Type::Bytes::ArrayBuffer) {
      return Linear{Term::lengthOf(var), 0};
    }
  }
  // [numerator, denominator]
  if (auto *call = dyn_cast<CallExpr>(object); call && call->builtin == Builtin::DecToFraction) {
    return Linear{Term::zero(), 2};
  }
  // `new Uint8Array([1, 2])`, `new Uint8Array(4)`, embedBytes: lengths that
  // never change.
  if (auto *call = dyn_cast<CallExpr>(object); call && call->builtin == Builtin::BytesOf) {
    return Linear{Term::zero(), static_cast<int64_t>(call->args.size())};
  }
  if (auto *call = dyn_cast<CallExpr>(object); call && call->builtin == Builtin::EmbedBytes) {
    return Linear{Term::zero(),
                  static_cast<int64_t>(cast<StringLitExpr>(call->args[0])->value.size())};
  }
  if (auto *call = dyn_cast<CallExpr>(object); call && call->builtin == Builtin::BytesNew) {
    if (std::optional<Linear> n = linear(call->args[0]); n && n->term.isZero()) {
      return n;
    }
  }

  return std::nullopt;
}

bool IndexProofs::provesInBounds(const Expr *object, const Expr *index, bool allowEnd) const {
  std::optional<Linear> at = linear(index), length = lengthOf(object);
  if (!length) {
    return false;
  }
  // 0 <= at, that is 0 < at + 1; and at < length (+ 1 with allowEnd)
  if (at && sema_.flow_.facts.proves(Term::zero(), at->term, at->offset + 1) &&
      sema_.flow_.facts.proves(at->term, length->term,
                         length->offset - at->offset + (allowEnd ? 1 : 0))) {
    return true;
  }
  // An index whose values are known from its form, `table[x & 0xff]` or
  // `hex[b >> 4]`: the largest below the length.
  ValueRange range = sema_.byteProofs().valueRange(index);

  return range.lo && range.hi && *range.lo >= 0 &&
         sema_.flow_.facts.proves(Term::zero(), length->term,
                            length->offset - *range.hi + (allowEnd ? 1 : 0));
}

bool IndexProofs::provesAtInBounds(const Expr *object, const Expr *index) const {
  std::optional<Linear> at = linear(index), length = lengthOf(object);
  if (at && length && at->term.isZero() && at->offset < 0) {
    // `at(-k)` reads length - k, which needs length >= k.
    return sema_.flow_.facts.proves(Term::zero(), length->term, length->offset + at->offset + 1);
  }

  return provesInBounds(object, index, /*allowEnd=*/false);
}

bool IndexProofs::provesNonNegative(const Expr *expr) const {
  std::optional<Linear> value = linear(expr);

  return value && sema_.flow_.facts.proves(Term::zero(), value->term, value->offset + 1);
}

bool IndexProofs::provesNotEmpty(const Expr *object) const {
  std::optional<Linear> length = lengthOf(object);

  return length && sema_.flow_.facts.proves(Term::zero(), length->term, length->offset);
}

std::optional<Linear> IndexProofs::knownLength(const Expr *value) const {
  if (auto *create = dyn_cast<NewExpr>(value)) {
    if (create->args.size() == 1) {
      return linear(create->args[0]);
    }
    return Linear{Term::zero(), 0};
  }
  // A new typed array, Buffer or ArrayBuffer of a known length.
  if (auto *call = dyn_cast<CallExpr>(value)) {
    switch (call->builtin) {
    case Builtin::BytesNew:
    case Builtin::ArrayBufferNew:
    case Builtin::BufferAllocFill:
      return call->args.empty() ? Linear{Term::zero(), 0} : linear(call->args[0]);
    case Builtin::BytesOf:
    case Builtin::EmbedBytes:
      return lengthOf(call);
    case Builtin::BytesFromArray:
    case Builtin::BytesFromBytes:
      return lengthOf(call->args[0]);
    default:
      break;
    }
  }

  return lengthOf(value);
}

} // namespace tz
