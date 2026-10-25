// Binary data (docs/bytes.md): ArrayBuffer, the typed arrays, DataView,
// Buffer as node has it, TextEncoder and TextDecoder. Each is an object type
// of its own, whose objects the runtime makes (runtime/bytes.c, buffer.c).
//
// Elements are read and written as those of arrays are, with the proofs of
// indices of sema_flow.cpp: an element read without one may be undefined,
// and a write without one is an error, since a typed array never grows. And
// every value stored in an element must be proven to fit in it (section 3):
// JavaScript would cut `u8[0] = 300` to 44 without a word. The proof is the
// one of indices, with the ranges of values: literals, masks (`x & 0xff`,
// `x >>> 24`), checks above, elements of a narrower kind, Math.min and
// Math.max (see valueRange).

#include "sema/sema.h"

#include "../../runtime/encodings.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MathExtras.h"

#include <limits>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

using Bytes = Type::Bytes;
using Range = ValueRange;

constexpr int64_t kIntMin = std::numeric_limits<int32_t>::min();
constexpr int64_t kIntMax = std::numeric_limits<int32_t>::max();

const std::pair<llvm::StringLiteral, Bytes> kNames[] = {
    {"Uint8Array", Bytes::Uint8},     {"Int8Array", Bytes::Int8},
    {"Uint8ClampedArray", Bytes::Uint8Clamped}, {"Int16Array", Bytes::Int16},
    {"Uint16Array", Bytes::Uint16},   {"Int32Array", Bytes::Int32},
    {"Uint32Array", Bytes::Uint32},   {"Float16Array", Bytes::Float16},
    {"Float32Array", Bytes::Float32}, {"Float64Array", Bytes::Float64},
    {"Buffer", Bytes::Buffer},        {"ArrayBuffer", Bytes::ArrayBuffer},
    {"DataView", Bytes::DataView},    {"TextEncoder", Bytes::TextEncoder},
    {"TextDecoder", Bytes::TextDecoder},
};

// Buffer's encodings, numbered as in runtime/buffer.c.
enum Encoding { Utf8, Utf16le, Latin1, Ascii, Base64, Base64url, Hex };

std::optional<int> encodingNamed(llvm::StringRef name) {
  static const std::pair<llvm::StringLiteral, Encoding> names[] = {
      {"utf8", Utf8},       {"utf-8", Utf8},         {"utf16le", Utf16le}, {"utf-16le", Utf16le},
      {"ucs2", Utf16le},    {"ucs-2", Utf16le},      {"latin1", Latin1},   {"binary", Latin1},
      {"ascii", Ascii},     {"base64", Base64},      {"base64url", Base64url}, {"hex", Hex},
  };
  std::string lower = name.lower();
  for (const auto &[n, e] : names)
    if (n == lower)
      return int(e);
  return std::nullopt;
}

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);
  return out;
}

// Text known when compiling: a literal, or a template without values.
std::optional<std::u16string> literalText(const Expr *e) {
  if (auto *conv = dyn_cast<CastExpr>(e); conv && conv->isImplicit())
    e = conv->operand;
  if (auto *lit = dyn_cast<StringLitExpr>(e))
    return lit->value;
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty())
    return tmpl->quasis[0];
  return std::nullopt;
}

// The text of a value for hints: a name, a number, the source of an
// operation; "x" for anything else.
std::string valueText(const Expr *e) {
  while (auto *conv = dyn_cast<CastExpr>(e)) {
    if (!conv->isImplicit())
      break;
    e = conv->operand;
  }
  if (auto *name = dyn_cast<NameExpr>(e))
    return name->name.str();
  if (auto *lit = dyn_cast<IntLitExpr>(e))
    return std::to_string(lit->value);
  if (auto *bin = dyn_cast<BinaryExpr>(e); bin && !bin->source.empty())
    return "(" + bin->source.str() + ")";
  if (auto *index = dyn_cast<IndexExpr>(e)) {
    std::string object = valueText(index->object), position = valueText(index->index);
    if (object != "x" && position != "x")
      return object + "[" + position + "]";
  }
  return "x";
}

std::string rangeText(int64_t lo, int64_t hi) {
  return std::to_string(lo) + " to " + std::to_string(hi);
}

// How to cut a value to the bits of an element, as JavaScript does.
std::string wrapHint(Bytes kind, const std::string &value) {
  switch (kind) {
  case Bytes::Int8:
    return "(" + value + " << 24) >> 24";
  case Bytes::Int16:
    return "(" + value + " << 16) >> 16";
  case Bytes::Uint16:
    return value + " & 0xffff";
  case Bytes::Uint32:
    return value + " >>> 0";
  case Bytes::Int32:
    return value + " | 0";
  default:
    return value + " & 0xff";
  }
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
  if (a.lo && b.lo)
    r.lo = std::min(*a.lo, *b.lo);
  if (a.hi && b.hi)
    r.hi = std::max(*a.hi, *b.hi);
  return r;
}

// The smallest 2^n - 1 at or above v >= 0.
int64_t allOnes(int64_t v) {
  int64_t m = 0;
  while (m < v)
    m = m * 2 + 1;
  return m;
}

int64_t floorShift(int64_t v, int64_t n) {
  return v >= 0 ? v >> n : -((-v + (int64_t(1) << n) - 1) >> n);
}

// The range of `a op b`, where integers of the type (an int with `isInt`)
// wrap around: a result that may leave its range is unknown.
Range operate(BinaryOp op, const Range &a, const Range &b, bool isInt) {
  int64_t min = isInt ? kIntMin : std::numeric_limits<int64_t>::min();
  int64_t max = isInt ? kIntMax : std::numeric_limits<int64_t>::max();
  auto within = [&](Range r) -> Range {
    if (!known(r) || *r.lo < min || *r.hi > max)
      return {};
    return r;
  };
  std::optional<int64_t> count; // a constant right side
  if (b.lo && b.hi && *b.lo == *b.hi)
    count = *b.lo;
  switch (op) {
  case BinaryOp::BitAnd: {
    // With a side that is not negative, the result lies from 0 to it.
    Range r;
    for (const Range *side : {&a, &b})
      if (side->lo && *side->lo >= 0) {
        r.lo = 0;
        if (side->hi)
          r.hi = r.hi ? std::min(*r.hi, *side->hi) : *side->hi;
      }
    return r;
  }
  case BinaryOp::BitOr:
  case BinaryOp::BitXor:
    if (known(a) && known(b) && *a.lo >= 0 && *b.lo >= 0)
      return {op == BinaryOp::BitOr ? std::max(*a.lo, *b.lo) : 0,
              allOnes(std::max(*a.hi, *b.hi))};
    return {};
  case BinaryOp::Shl:
    if (count && known(a) && *count >= 0 && *count < (isInt ? 32 : 64) && *count < 40 &&
        *a.lo > -(int64_t(1) << 40) && *a.hi < (int64_t(1) << 40))
      return within({*a.lo * (int64_t(1) << *count), *a.hi * (int64_t(1) << *count)});
    return {};
  case BinaryOp::Shr: {
    if (!count)
      return {};
    int64_t n = *count & (isInt ? 31 : 63);
    if (known(a))
      return {floorShift(*a.lo, n), floorShift(*a.hi, n)};
    if (isInt)
      return {floorShift(kIntMin, n), floorShift(kIntMax, n)};
    return {};
  }
  case BinaryOp::UShr: {
    if (!count)
      return {};
    int64_t n = *count & (isInt ? 31 : 63);
    if (a.lo && *a.lo >= 0 && a.hi)
      return {*a.lo >> n, *a.hi >> n};
    if (isInt)
      return {0, int64_t(0xFFFFFFFF) >> n};
    if (n > 0)
      return {0, std::numeric_limits<int64_t>::max() >> (n - 1)};
    return {};
  }
  case BinaryOp::Rem: {
    if (!count || *count == 0)
      return {};
    int64_t m = (*count < 0 ? -*count : *count) - 1;
    if (a.lo && *a.lo >= 0)
      return {0, a.hi ? std::min(m, *a.hi) : m};
    if (a.hi && *a.hi <= 0)
      return {a.lo ? std::max(-m, *a.lo) : -m, 0};
    return {-m, m};
  }
  case BinaryOp::Div:
    if (count && *count > 0 && known(a))
      return {*a.lo / *count, *a.hi / *count};
    return {};
  case BinaryOp::Add:
  case BinaryOp::Sub:
  case BinaryOp::Mul: {
    if (!known(a) || !known(b))
      return {};
    int64_t xs[2] = {*a.lo, *a.hi}, ys[2] = {*b.lo, *b.hi};
    int64_t lo = std::numeric_limits<int64_t>::max(), hi = std::numeric_limits<int64_t>::min();
    for (int64_t x : xs)
      for (int64_t y : ys) {
        int64_t v;
        bool overflow = op == BinaryOp::Add   ? llvm::AddOverflow(x, y, v)
                        : op == BinaryOp::Sub ? llvm::SubOverflow(x, y, v)
                                              : llvm::MulOverflow(x, y, v);
        if (overflow)
          return {};
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
    // The extremes are among the corners: for `a - b`, lo(a) - hi(b) and
    // hi(a) - lo(b).
    return within({lo, hi});
  }
  default:
    return {};
  }
}

} // namespace

const Type *Sema::bytesTypeNamed(llvm::StringRef name) {
  for (const auto &[n, kind] : kNames)
    if (n == name)
      return Type::bytesType(kind);
  return nullptr;
}

//===----------------------------------------------------------------------===//
// What values fit
//===----------------------------------------------------------------------===//

ValueRange Sema::valueRange(const Expr *e) const {
  // Of `T | undefined`, what it holds when it holds a value: `u[i] ?? 0`.
  if (!e->type || !e->type->nonOptional()->isInteger())
    return {};
  if (std::optional<int64_t> c = intConstant(e))
    return exact(*c);
  bool isInt = e->type->nonOptional()->isInt();
  switch (e->kind()) {
  case Expr::Kind::Cast: {
    auto *conv = cast<CastExpr>(e);
    if (!conv->isImplicit() || !conv->operand->type || !conv->operand->type->isInteger())
      return {};
    Range r = valueRange(conv->operand);
    // A long cut to its low 32 bits keeps its value if it fits in an int.
    if (isInt && conv->operand->type->kind() == Type::Kind::Long && !(known(r) && *r.lo >= kIntMin && *r.hi <= kIntMax))
      return {};
    return r;
  }
  case Expr::Kind::Name: {
    const VarSymbol *var = cast<NameExpr>(e)->var;
    if (!var || !var->type->isInt())
      return {};
    // The bounds the facts give it: `x < 256`, `0 <= x`, `i < u.length` of
    // a u whose length is known.
    Range r;
    Term self = Term::of(var);
    if (std::optional<int64_t> above = flow_.facts.bound(self, Term::zero()))
      r.hi = std::min(*above - 1, kIntMax); // x < above
    if (std::optional<int64_t> below = flow_.facts.bound(Term::zero(), self))
      r.lo = std::max(1 - *below, kIntMin); // 0 < x + below
    return r;
  }
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(e);
    if (member->name == "length" || member->name == "byteLength")
      return {0, kMaxLength};
    return {};
  }
  case Expr::Kind::Index: {
    // An element holds what its kind holds.
    auto *index = cast<IndexExpr>(e);
    int64_t lo, hi;
    if (index->object->type && index->object->type->elementRange(lo, hi))
      return {lo, hi};
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
      if (cast<MemberExpr>(call->callee)->object->type->elementRange(lo, hi))
        return {lo, hi};
      return {};
    case Builtin::DataViewGet:
    case Builtin::BufferRead:
      if (Type::bytesType(Bytes(cast<IntLitExpr>(call->args[0])->value))->elementRange(lo, hi))
        return {lo, hi};
      return {};
    case Builtin::MathMin:
    case Builtin::MathMax: {
      if (call->hasSpread() || call->args.empty())
        return {};
      bool min = call->builtin == Builtin::MathMin;
      Range r = valueRange(call->args[0]);
      for (size_t i = 1; i < call->args.size(); ++i) {
        Range next = valueRange(call->args[i]);
        Range both;
        // min: below each upper bound; above the lowest of the lower ones.
        if (min) {
          if (r.hi || next.hi)
            both.hi = r.hi && next.hi ? std::min(*r.hi, *next.hi) : r.hi ? *r.hi : *next.hi;
          if (r.lo && next.lo)
            both.lo = std::min(*r.lo, *next.lo);
        } else {
          if (r.lo || next.lo)
            both.lo = r.lo && next.lo ? std::max(*r.lo, *next.lo) : r.lo ? *r.lo : *next.lo;
          if (r.hi && next.hi)
            both.hi = std::max(*r.hi, *next.hi);
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
    if (unary->op == UnaryOp::Plus)
      return r;
    if (!known(r))
      return {};
    if (unary->op == UnaryOp::Neg)
      return operate(BinaryOp::Sub, exact(0), r, isInt);
    if (unary->op == UnaryOp::BitNot)
      return {-*r.hi - 1, -*r.lo - 1};
    return {};
  }
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(e);
    if (bin->op == BinaryOp::Nullish)
      return unite(valueRange(bin->lhs), valueRange(bin->rhs));
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

bool Sema::provesFits(const Expr *expr, int64_t lo, int64_t hi) const {
  // An int widened to a long keeps its value.
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit() || !conv->operand->type || !conv->operand->type->isInt() ||
        !conv->type->isInteger())
      break;
    expr = conv->operand;
  }
  if (auto *cond = dyn_cast<ConditionalExpr>(expr))
    return provesFits(cond->thenExpr, lo, hi) && provesFits(cond->elseExpr, lo, hi);
  bool lower = false, upper = false;
  // `x` or `x + k` with facts about x: `if (x >= 0 && x <= 255)`.
  if (std::optional<Linear> l = linear(expr)) {
    if (l->term.isZero()) {
      lower = l->offset >= lo;
      upper = l->offset <= hi;
    } else {
      lower = flow_.facts.proves(Term::zero(), l->term, l->offset - lo + 1);
      upper = flow_.facts.proves(l->term, Term::zero(), hi - l->offset + 1);
    }
  }
  Range r = valueRange(expr);
  lower |= r.lo && *r.lo >= lo;
  upper |= r.hi && *r.hi <= hi;
  return lower && upper;
}

void Sema::checkFits(const Expr *value, const Type *target, llvm::StringRef what) {
  int64_t lo, hi;
  // A floating element holds any number, losing precision only; a value
  // that is no integer was reported where it was converted.
  if (!value->type->isInteger() || !target->elementRange(lo, hi))
    return;
  Bytes kind = target->bytes();
  // A clamped element takes any int (that is what it is for), and one of an
  // Int32Array is an int.
  if (kind == Bytes::Uint8Clamped || kind == Bytes::Int32 || provesFits(value, lo, hi))
    return;
  std::string text = valueText(value);
  if (std::optional<int64_t> c = intConstant(value)) {
    diags_.error(value->loc(), std::to_string(*c) + " does not fit in " + what.str() + " (" +
                                   rangeText(lo, hi) + "): JavaScript would store " +
                                   std::to_string(wrapped(kind, *c)) +
                                   "; write a value that fits, or cut it explicitly: '" +
                                   wrapHint(kind, std::to_string(*c)) + "'");
    return;
  }
  diags_.error(value->loc(),
               "cannot prove that the value fits in " + what.str() + " (" + rangeText(lo, hi) +
                   "), and JavaScript would cut it without a word; check it first, e.g. 'if (" +
                   text + " >= " + std::to_string(lo) + " && " + text + " <= " +
                   std::to_string(hi) + ")', or cut it explicitly: '" + wrapHint(kind, text) + "'");
}

bool Sema::provesWithin(const Expr *object, const Expr *offset, int64_t size) const {
  std::optional<Linear> at = offset ? linear(offset) : Linear{Term::zero(), 0};
  std::optional<Linear> length = lengthOf(object);
  if (!at || !length)
    return false;
  // 0 <= at, and at + size <= length, that is at < length - size + 1.
  return flow_.facts.proves(Term::zero(), at->term, at->offset + 1) &&
         flow_.facts.proves(at->term, length->term, length->offset - size + 1 - at->offset);
}

void Sema::addByteCallbackFacts(const FunctionDecl *fn) {
  auto it = byteCallbacks_.find(fn);
  if (it == byteCallbacks_.end() || it->second.param >= fn->params.size())
    return;
  const ByteCallback &info = it->second;
  const VarSymbol *var = fn->params[info.param].sym;
  if (!var || !var->type->isInt() || (info.lo <= kIntMin && info.hi >= kIntMax))
    return;
  // lo <= x and x <= hi, as the facts write them.
  flow_.facts.add(Term::zero(), Term::of(var), 1 - info.lo);
  flow_.facts.add(Term::of(var), Term::zero(), info.hi + 1);
}

void Sema::checkByteCallbackResult(const FunctionDecl *fn, const Expr *value) {
  auto it = byteCallbacks_.find(fn);
  if (it == byteCallbacks_.end() || !it->second.target)
    return;
  checkFits(value, it->second.target, "an element of '" + it->second.target->name().str() + "'");
}

//===----------------------------------------------------------------------===//
// Elements
//===----------------------------------------------------------------------===//

Expr *Sema::checkBytesIndex(IndexExpr *index) {
  const Type *type = index->object->type;
  index->index = checkPosition(index->index);
  if (!type->isTypedArray()) {
    diags_.error(index->loc(), "type '" + type->name() + "' cannot be indexed; " +
                                   (type->bytes() == Bytes::DataView
                                        ? "read its bytes with getUint8(i) and the like"
                                        : type->bytes() == Bytes::ArrayBuffer
                                              ? "read its bytes through a view, e.g. 'new "
                                                "Uint8Array(buffer)[i]'"
                                              : "it holds no elements"));
    return index;
  }
  if (index->index->type->isError())
    return index;
  // Proved in range: a plain read. Otherwise JavaScript yields undefined.
  bool inRange = provesInBounds(index->object, index->index, /*allowEnd=*/false);
  index->mayBeOutOfRange = !inRange;
  index->type = inRange ? type->elementType() : Type::optional(type->elementType());
  return index;
}

void Sema::checkBytesStore(IndexExpr *index, const Expr *value, std::optional<BinaryOp> op,
                           llvm::SMLoc loc) {
  const Type *target = index->object->type;
  if (index->index->type->isError() || (value && value->type->isError()))
    return;
  std::string object = valueText(index->object), position = valueText(index->index);
  if (!provesInBounds(index->object, index->index, /*allowEnd=*/false)) {
    std::optional<int64_t> at = intConstant(index->index);
    std::string check = at ? object + ".length > " + std::to_string(*at)
                           : position + " >= 0 && " + position + " < " + object + ".length";
    diags_.error(index->loc(), "cannot prove that the index is within the " + target->name() +
                                   " (0 <= index < length): it never grows, and JavaScript would "
                                   "drop the write; check the index first, e.g. 'if (" + check +
                                   ")'");
    return;
  }
  std::string what = "an element of '" + target->name().str() + "'";
  if (!op) {
    checkFits(value, target, what);
    return;
  }
  // `u[i] op= v` stores `u[i] op v`, and `u[i]++` stores u[i] + 1.
  int64_t lo, hi;
  if (!target->elementRange(lo, hi) || target->bytes() == Bytes::Uint8Clamped ||
      target->bytes() == Bytes::Int32)
    return;
  Range result = operate(*op, Range{lo, hi}, value ? valueRange(value) : exact(1),
                         target->elementType()->isInt());
  if (result.lo && result.hi && *result.lo >= lo && *result.hi <= hi)
    return;
  std::string element = object + "[" + position + "]";
  std::string written = value ? element + " " + opSpelling(*op) + " " + valueText(value)
                              : element + (*op == BinaryOp::Add ? " + 1" : " - 1");
  diags_.error(loc, "cannot prove that the result fits in " + what + " (" + rangeText(lo, hi) +
                        "), and JavaScript would cut it without a word; cut it explicitly: '" +
                        element + " = " + wrapHint(target->bytes(), "(" + written + ")") + "'");
}

//===----------------------------------------------------------------------===//
// Making them
//===----------------------------------------------------------------------===//

Expr *Sema::checkBytesNew(NewExpr *expr, const Type *type) {
  llvm::SMLoc loc = expr->loc();
  std::vector<Expr *> &args = expr->args;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : args)
      arg = checkExpr(arg);
  };
  auto *callee = ctx_.make<NameExpr>(loc, type->name());
  // A fake call, for the helpers that check arguments.
  auto *call = ctx_.make<CallExpr>(loc, callee, args);
  if (!expr->typeArgs.empty())
    diags_.error(loc, "'new " + type->name() + "' takes no type arguments");
  auto made = [&](Builtin builtin, const Type *result) {
    call->builtin = builtin;
    call->type = result;
    return call;
  };
  call->type = Type::error();
  switch (type->bytes()) {
  case Bytes::Buffer:
    diags_.error(loc, "'new Buffer(...)' is not supported, as in node, where it is deprecated: "
                      "make one with 'Buffer.from(values)' or 'Buffer.alloc(size)'");
    checkArgsLoosely();
    return expr;
  case Bytes::ArrayBuffer:
    if (args.size() == 2) {
      diags_.error(args[1]->loc(), "an ArrayBuffer that can change its size is not supported yet "
                                   "(docs/bytes.md, section 7); make a new one of the size wanted");
      checkArgsLoosely();
      return expr;
    }
    if (!checkArity(call, loc, 1, 1))
      return call;
    call->args[0] = checkPosition(call->args[0]);
    if (std::optional<Linear> n = linear(call->args[0]); n && n->term.isZero() && n->offset < 0)
      diags_.error(call->args[0]->loc(), "the length of an ArrayBuffer cannot be negative");
    return made(Builtin::ArrayBufferNew, type);
  case Bytes::DataView: {
    if (!checkArity(call, loc, 1, 3))
      return call;
    call->args[0] = checkExpr(call->args[0]);
    const Type *buffer = call->args[0]->type;
    if (buffer->bytes() != Bytes::ArrayBuffer && !buffer->isError()) {
      diags_.error(call->args[0]->loc(),
                   "a DataView is made over an ArrayBuffer, found '" + buffer->name() + "'" +
                       (buffer->isTypedArray() ? "; give its buffer: 'new DataView(u.buffer, "
                                                 "u.byteOffset, u.byteLength)'"
                                               : ""));
      return call;
    }
    for (size_t i = 1; i < call->args.size(); ++i)
      call->args[i] = checkPosition(call->args[i]);
    return made(Builtin::DataViewNew, type);
  }
  case Bytes::TextEncoder:
    if (!checkArity(call, loc, 0, 0))
      return call;
    return made(Builtin::TextEncoderNew, type);
  case Bytes::TextDecoder: {
    if (!checkArity(call, loc, 0, 2))
      return call;
    // The label: one written in the code is looked up now, as its table is
    // built into the program only then (docs/bytes.md, section 5).
    auto *resolved = ctx_.make<IntLitExpr>(loc, 1);
    resolved->type = Type::intTy();
    std::u16string name = u"utf-8";
    std::vector<Expr *> checked{resolved, nullptr};
    if (!call->args.empty() && !isa<UndefinedExpr>(call->args[0])) {
      Expr *label = call->args[0];
      if (std::optional<std::u16string> text = literalText(label)) {
        std::string lower = llvm::StringRef(utf8(*text)).trim(" \t\n\f\r").lower();
        const char *found = tz_encoding_name(lower.c_str());
        if (!found) {
          diags_.error(label->loc(), "'" + utf8(*text) +
                                         "' is not an encoding TextDecoder knows (JavaScript "
                                         "throws a RangeError); e.g. 'utf-8', 'utf-16le', "
                                         "'windows-1251', 'latin1'");
          return call;
        }
        if (tz_encoding_multi_byte(found)) {
          diags_.error(label->loc(), "the encoding '" + std::string(found) +
                                         "' is not supported yet: of the old encodings, only "
                                         "those of one byte per character are");
          return call;
        }
        name = std::u16string(found, found + strlen(found));
      } else {
        // Known only when the program runs: the program then holds the
        // tables of all the encodings.
        call->args[0] = checkArg(label, Type::string());
        resolved->value = 0;
        checked[1] = call->args[0];
      }
    } else if (!call->args.empty()) {
      call->args[0] = checkExpr(call->args[0]);
    }
    if (!checked[1]) {
      auto *lit = ctx_.make<StringLitExpr>(loc, name);
      lit->type = Type::string();
      checked[1] = lit;
    }
    if (call->args.size() == 2) {
      const Type *options = Type::object({{"fatal", Type::optional(Type::boolTy()), false},
                                          {"ignoreBOM", Type::optional(Type::boolTy()), false}});
      Expr *given = checkExpr(call->args[1], options);
      checked.push_back(coerce(given, options, "the options of TextDecoder"));
    }
    call->args = checked;
    return made(Builtin::TextDecoderNew, type);
  }
  default:
    break;
  }

  // A typed array: of a length, of values, of another typed array, or a view
  // of an ArrayBuffer.
  if (args.empty()) {
    auto *zero = ctx_.make<IntLitExpr>(loc, 0);
    zero->type = Type::longTy();
    call->args = {zero};
    return made(Builtin::BytesNew, type);
  }
  if (args.size() > 3) {
    checkArity(call, loc, 0, 3);
    return call;
  }
  Expr *&source = call->args[0];
  if (auto *lit = dyn_cast<ArrayLitExpr>(source); lit && args.size() == 1)
    return checkBytesValues(call, lit, type);
  markFused(source);
  source = checkExpr(source);
  const Type *from = source->type;
  if (from->isError())
    return call;
  if (from->bytes() == Bytes::ArrayBuffer) {
    for (size_t i = 1; i < call->args.size(); ++i)
      call->args[i] = checkPosition(call->args[i]);
    return made(Builtin::BytesView, type);
  }
  if (call->args.size() > 1) {
    diags_.error(call->args[1]->loc(), "'new " + type->name() +
                                           "' takes an offset and a length only with an "
                                           "ArrayBuffer, found '" + from->name() + "'");
    return call;
  }
  if (from->isInteger()) {
    source = convert(source, Type::longTy());
    if (std::optional<Linear> n = linear(source); n && n->term.isZero() && n->offset < 0)
      diags_.error(source->loc(), "the length of a typed array cannot be negative");
    auto *zeroed = ctx_.make<BoolLitExpr>(loc, true);
    zeroed->type = Type::boolTy();
    call->args.push_back(zeroed);
    return made(Builtin::BytesNew, type);
  }
  if (from->isFloat()) {
    diags_.error(source->loc(), "the length of a typed array must be an integer, found 'double'; "
                                "round it with Math.floor(x) or drop the fraction with 'x | 0'");
    return call;
  }
  if (!checkBytesSource(source, type))
    return call;
  return made(from->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray, type);
}

Expr *Sema::checkBytesValues(CallExpr *call, ArrayLitExpr *lit, const Type *type) {
  const Type *element = type->elementType();
  std::string what = "an element of '" + type->name().str() + "'";
  call->type = Type::error();
  bool spreads = llvm::any_of(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread; });
  if (!spreads) {
    std::vector<Expr *> values;
    for (ArrayLitExpr::Element &e : lit->elements) {
      if (e.hole) {
        diags_.error(e.expr->loc(), "arrays cannot have holes; write the element explicitly");
        return call;
      }
      Expr *value = checkArg(e.expr, element);
      checkFits(value, type, what);
      values.push_back(value);
    }
    call->args = std::move(values);
    call->builtin = Builtin::BytesOf;
    call->type = type;
    return call;
  }
  // With spreads, an array of the values first.
  Expr *array = checkExpr(lit, Type::array(element));
  if (array->type->isError())
    return call;
  bool ok = true;
  for (const ArrayLitExpr::Element &e : lit->elements) {
    if (!e.spread)
      checkFits(e.expr, type, what);
    else
      ok &= checkBytesSource(e.expr, type);
  }
  if (!ok)
    return call;
  call->args = {array};
  call->builtin = Builtin::BytesFromArray;
  call->type = type;
  return call;
}

bool Sema::checkBytesSource(Expr *source, const Type *target) {
  const Type *from = source->type;
  if (from->isError())
    return false;
  const Type *element = from->isTypedArray() ? from->elementType()
                        : from->isArray()    ? from->inner()
                        : from->isIterator() ? from->yielded()
                                             : nullptr;
  if (!element) {
    diags_.error(source->loc(), "a " + target->name() +
                                    " is made from a length, an array or an iterator of numbers, "
                                    "another typed array or an ArrayBuffer, found '" +
                                    from->name() + "'" +
                                    (from->isString() ? "; for the bytes of a string, use 'new "
                                                        "TextEncoder().encode(s)' or "
                                                        "'Buffer.from(s)'"
                                                      : ""));
    return false;
  }
  if (from->isIterator())
    checkSendsNothing(from, source->loc(), "'" + target->name().str() + "'");
  if (!element->isNumeric()) {
    diags_.error(source->loc(), "the elements of '" + from->name() + "' are not numbers, which a " +
                                    target->name() + " holds");
    return false;
  }
  int64_t lo, hi;
  if (!target->elementRange(lo, hi))
    return true; // a floating element takes any number
  Bytes kind = target->bytes();
  std::string text = valueText(source);
  std::string hint =
      target->name().str() + ".from(" + text + ", x => " + wrapHint(kind, "x") + ")";
  if (element->isFloat()) {
    diags_.error(source->loc(), "the elements of '" + from->name() + "' are doubles, which an "
                                    "element of '" + target->name() +
                                    "' cannot hold without an explicit conversion; convert them, "
                                    "e.g. '" + target->name() + ".from(" + text +
                                    ", x => Math.round(x) & 0xff)'");
    return false;
  }
  // An Int32Array takes any int, and a clamped element any integer.
  if (kind == Bytes::Uint8Clamped || (kind == Bytes::Int32 && element->isInt()))
    return true;
  int64_t slo, shi;
  if (from->isTypedArray() && from->elementRange(slo, shi)) {
    if (slo >= lo && shi <= hi)
      return true;
    diags_.error(source->loc(), "the elements of '" + from->name() + "' (" + rangeText(slo, shi) +
                                    ") may not fit in an element of '" + target->name() + "' (" +
                                    rangeText(lo, hi) +
                                    "), and JavaScript would cut them without a word; convert "
                                    "them explicitly, e.g. '" + hint + "'");
    return false;
  }
  diags_.error(source->loc(), "cannot prove that the elements of '" + from->name() + "' fit in " +
                                  "an element of '" + target->name() + "' (" + rangeText(lo, hi) +
                                  "), and JavaScript would cut them without a word; write the "
                                  "values in the literal, or convert them explicitly, e.g. '" +
                                  hint + "'");
  return false;
}

//===----------------------------------------------------------------------===//
// Properties
//===----------------------------------------------------------------------===//

namespace {

const llvm::StringLiteral kTypedMethods[] = {
    "at",       "copyWithin", "entries",     "every",      "fill",    "filter",  "find",
    "findIndex", "findLast",  "findLastIndex", "forEach",  "includes", "indexOf", "join",
    "keys",     "lastIndexOf", "map",        "reduce",     "reduceRight", "reverse", "set",
    "slice",    "some",       "sort",        "subarray",   "toReversed", "toSorted", "toString",
    "values",   "with"};

} // namespace

Expr *Sema::checkBytesMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  auto read = [&](Builtin builtin, const Type *result, std::vector<Expr *> args = {}) {
    return builtinCall(member->loc(), member, builtin, std::move(args), result);
  };
  // A constant, after the object for what evaluating it does.
  auto constant = [&](Expr *value) -> Expr * {
    if (isa<NameExpr>(member->object))
      return value;
    auto *seq = ctx_.make<SequenceExpr>(member->loc(), std::vector<Expr *>{member->object, value});
    seq->type = value->type;
    return seq;
  };
  auto intLit = [&](int64_t v) {
    auto *lit = ctx_.make<IntLitExpr>(member->loc(), uint64_t(v));
    lit->type = Type::intTy();
    return lit;
  };
  auto boolLit = [&](bool v) {
    auto *lit = ctx_.make<BoolLitExpr>(member->loc(), v);
    lit->type = Type::boolTy();
    return lit;
  };
  switch (type->bytes()) {
  case Bytes::ArrayBuffer:
    if (name == "byteLength") {
      member->type = Type::intTy(); // read by CodeGen, and a length for the proofs
      return member;
    }
    if (name == "maxByteLength")
      return read(Builtin::BytesByteLength, Type::intTy());
    if (name == "resizable" || name == "detached")
      return constant(boolLit(false));
    break;
  case Bytes::DataView:
    if (name == "byteLength") {
      member->type = Type::intTy();
      return member;
    }
    if (name == "byteOffset")
      return read(Builtin::BytesByteOffset, Type::intTy());
    if (name == "buffer")
      return read(Builtin::BytesBuffer, Type::bytesType(Bytes::ArrayBuffer));
    break;
  case Bytes::TextEncoder:
    if (name == "encoding") {
      auto *lit = ctx_.make<StringLitExpr>(member->loc(), u"utf-8");
      lit->type = Type::string();
      return constant(lit);
    }
    break;
  case Bytes::TextDecoder:
    if (name == "encoding")
      return read(Builtin::TextDecoderInfo, Type::string(), {intLit(0)});
    if (name == "fatal" || name == "ignoreBOM")
      return read(Builtin::TextDecoderInfo, Type::boolTy(), {intLit(name == "fatal" ? 1 : 2)});
    break;
  default:
    if (name == "length") {
      member->type = Type::intTy();
      return member;
    }
    if (name == "byteLength")
      return read(Builtin::BytesByteLength, Type::intTy());
    if (name == "byteOffset")
      return read(Builtin::BytesByteOffset, Type::intTy());
    if (name == "buffer")
      return read(Builtin::BytesBuffer, Type::bytesType(Bytes::ArrayBuffer));
    if (name == "BYTES_PER_ELEMENT")
      return constant(intLit(type->elementSize()));
    if (type->isBuffer() && (name == "parent" || name == "offset")) {
      diags_.error(member->loc(), "'" + name + "' of a Buffer is deprecated in node and not "
                                  "supported; use '" + (name == "parent" ? "buffer" : "byteOffset") + "'");
      return member;
    }
    if (llvm::is_contained(kTypedMethods, name)) {
      diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                      type->name() + "'; methods must be called");
      return member;
    }
    break;
  }
  diags_.error(member->loc(), "property '" + name + "' does not exist on type '" + type->name() + "'");
  return member;
}

Expr *Sema::checkBytesStaticMember(MemberExpr *member) {
  auto *object = dyn_cast<NameExpr>(member->object);
  if (!object || !isGlobalName(object, object->name))
    return nullptr;
  const Type *type = bytesTypeNamed(object->name);
  if (!type)
    return nullptr;
  member->type = Type::error();
  if (type->isTypedArray() && member->name == "BYTES_PER_ELEMENT") {
    auto *lit = ctx_.make<IntLitExpr>(member->loc(), type->elementSize());
    lit->type = Type::intTy();
    return lit;
  }
  // The size of the pool of Buffer.allocUnsafe in node, which tz has none of.
  if (type->isBuffer() && member->name == "poolSize") {
    auto *lit = ctx_.make<IntLitExpr>(member->loc(), 8192);
    lit->type = Type::intTy();
    return lit;
  }
  diags_.error(member->loc(), "'" + object->name + "." + member->name +
                                  "' can be used only in a call, e.g. '" + object->name + "." +
                                  member->name + "(...)'");
  return member;
}

Expr *Sema::bytesToString(Expr *expr) {
  const Type *type = expr->type;
  if (type->isTypedArray()) {
    auto *callee = ctx_.make<MemberExpr>(expr->loc(), expr, "toString");
    if (type->isBuffer()) {
      auto *encoding = ctx_.make<IntLitExpr>(expr->loc(), Utf8);
      encoding->type = Type::intTy();
      return builtinCall(expr->loc(), callee, Builtin::BufferToString, {encoding}, Type::string());
    }
    return builtinCall(expr->loc(), callee, Builtin::BytesJoin, {}, Type::string());
  }
  diags_.error(expr->loc(), "a value of type '" + type->name() +
                                "' cannot be converted to a string: JavaScript would give '[object " +
                                type->name() + "]'; print it with console.log" +
                                (type->bytes() == Bytes::ArrayBuffer
                                     ? ", or the text of its bytes with 'new TextDecoder().decode(buffer)'"
                                     : ""));
  return expr;
}

//===----------------------------------------------------------------------===//
// Methods
//===----------------------------------------------------------------------===//

Expr *Sema::checkBufferEncoding(Expr *arg) {
  if (std::optional<std::u16string> text = literalText(arg)) {
    std::optional<int> encoding = encodingNamed(utf8(*text));
    if (!encoding) {
      diags_.error(arg->loc(), "unknown encoding '" + utf8(*text) +
                                   "' (node throws a TypeError); Buffer knows utf8, utf16le, "
                                   "latin1, ascii, base64, base64url and hex");
      arg->type = Type::error();
      return arg;
    }
    auto *lit = ctx_.make<IntLitExpr>(arg->loc(), uint64_t(*encoding));
    lit->type = Type::intTy();
    return lit;
  }
  // Checked when the program runs: an unknown one is a TypeError.
  return checkArg(arg, Type::string());
}

Expr *Sema::checkBytesMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *type = member->object->type;
  call->type = Type::error();
  if (rejectSpread(call))
    return call;
  switch (type->bytes()) {
  case Bytes::ArrayBuffer: {
    llvm::StringRef name = member->name;
    if (name == "slice") {
      if (!checkArity(call, member->loc(), 0, 2))
        return call;
      for (Expr *&arg : call->args)
        arg = checkPosition(arg);
      call->builtin = Builtin::ArrayBufferSlice;
      call->type = type;
      return call;
    }
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    if (name == "resize" || name == "transfer" || name == "transferToFixedLength")
      diags_.error(member->loc(), "'" + name + "' is not supported yet: an ArrayBuffer keeps its "
                                      "size and its memory (docs/bytes.md, section 7)");
    else
      diags_.error(member->loc(), "property '" + name + "' does not exist on type 'ArrayBuffer'");
    return call;
  }
  case Bytes::DataView:
    return checkDataViewMethod(call, member);
  case Bytes::TextEncoder:
  case Bytes::TextDecoder:
    return checkTextMethod(call, member);
  default:
    break;
  }
  if (type->isBuffer())
    if (Expr *result = checkBufferMethod(call, member))
      return result;
  return checkTypedArrayMethod(call, member, expected);
}

Expr *Sema::checkTypedArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *type = member->object->type;
  const Type *element = type->elementType();
  llvm::StringRef name = member->name;
  std::string what = "an element of '" + type->name().str() + "'";
  // What toReversed, toSorted and with make: a Uint8Array of a Buffer, as
  // TypedArrayCreateSameType makes.
  const Type *copy = type->isBuffer() ? Type::bytesType(Bytes::Uint8) : type;
  auto made = [&](Builtin builtin, const Type *result) {
    call->builtin = builtin;
    call->type = result;
    return call;
  };
  auto positions = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i)
      call->args[i] = checkPosition(call->args[i]);
  };
  auto mode = [&](int value) {
    auto *lit = ctx_.make<IntLitExpr>(call->loc(), uint64_t(value));
    lit->type = Type::intTy();
    return lit;
  };

  static const llvm::StringLiteral callbacks[] = {
      "forEach", "map", "filter", "reduce", "reduceRight", "find", "findLast", "findIndex",
      "findLastIndex", "some", "every"};
  if (llvm::is_contained(callbacks, name) ||
      ((name == "sort" || name == "toSorted") && !call->args.empty())) {
    bool arrow = !call->args.empty() && isa<FunctionExpr>(call->args[0]);
    Expr *result = checkCallbackMethod(call, member, expected);
    int64_t lo, hi;
    if (name == "map" && !call->args.empty() && !arrow &&
        call->args[0]->type->isFunction() && type->elementRange(lo, hi) &&
        type->bytes() != Bytes::Uint8Clamped && type->bytes() != Bytes::Int32)
      diags_.error(call->args[0]->loc(),
                   "cannot prove that what the function returns fits in " + what + " (" +
                       rangeText(lo, hi) +
                       "), and JavaScript would cut it without a word; pass an arrow that cuts it "
                       "explicitly, e.g. 'x => " + wrapHint(type->bytes(), "f(x)") + "'");
    if (name == "toSorted" && !call->type->isError())
      call->type = copy;
    return result;
  }
  if (name == "flatMap") {
    diags_.error(member->loc(), "typed arrays have no 'flatMap'; spread the elements into an "
                                "array first, e.g. '[...u].flatMap(f)'");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  if (name == "at") {
    if (!checkArity(call, member->loc(), 1, 1))
      return call;
    positions(0);
    bool inRange = provesAtInBounds(member->object, call->args[0]);
    call->mayBeOutOfRange = !inRange;
    return made(Builtin::BytesAt, inRange ? element : Type::optional(element));
  }
  if (name == "indexOf" || name == "lastIndexOf" || name == "includes") {
    if (!checkArity(call, member->loc(), 1, 2))
      return call;
    Expr *value = numericArg(call->args[0], element);
    if (!value->type->isNumeric())
      return call;
    std::vector<Expr *> args{mode(name == "indexOf" ? 0 : name == "lastIndexOf" ? 1 : 2),
                             convert(value, Type::doubleTy())};
    if (call->args.size() == 2)
      args.push_back(checkPosition(call->args[1]));
    call->args = std::move(args);
    return made(Builtin::BytesIndexOf, name == "includes" ? Type::boolTy() : Type::intTy());
  }
  if (name == "join" || name == "toString") {
    if (!checkArity(call, member->loc(), 0, name == "join" ? 1 : 0))
      return call;
    if (!call->args.empty())
      call->args[0] = checkArg(call->args[0], Type::string());
    return made(Builtin::BytesJoin, Type::string());
  }
  if (name == "reverse" || name == "sort" || name == "toReversed" || name == "toSorted") {
    if (!checkArity(call, member->loc(), 0, 0))
      return call;
    if (name == "reverse")
      return made(Builtin::BytesReverse, type);
    if (name == "sort")
      return made(Builtin::BytesSort, type);
    return made(name == "toReversed" ? Builtin::BytesToReversed : Builtin::BytesToSorted, copy);
  }
  if (name == "fill") {
    if (!checkArity(call, member->loc(), 1, 3))
      return call;
    call->args[0] = checkArg(call->args[0], element);
    checkFits(call->args[0], type, what);
    call->args[0] = convert(call->args[0], Type::doubleTy());
    positions(1);
    return made(Builtin::BytesFill, type);
  }
  if (name == "set") {
    if (!checkArity(call, member->loc(), 1, 2))
      return call;
    Expr *&source = call->args[0];
    if (auto *lit = dyn_cast<ArrayLitExpr>(source)) {
      source = checkExpr(lit, Type::array(element));
      if (source->type->isError())
        return call;
      for (const ArrayLitExpr::Element &e : lit->elements) {
        if (!e.spread)
          checkFits(e.expr, type, what);
        else if (!checkBytesSource(e.expr, type))
          return call;
      }
    } else {
      source = checkExpr(source);
      if (source->type->isIterator()) {
        diags_.error(source->loc(), "'set' takes an array or a typed array; collect the values "
                                    "of the iterator first, e.g. '[...it]'");
        return call;
      }
      if (!checkBytesSource(source, type))
        return call;
    }
    positions(1);
    return made(Builtin::BytesSet, Type::voidTy());
  }
  if (name == "subarray" || name == "slice") {
    if (!checkArity(call, member->loc(), 0, 2))
      return call;
    positions(0);
    return made(name == "subarray" ? Builtin::BytesSubarray : Builtin::BytesSlice, type);
  }
  if (name == "copyWithin") {
    if (!checkArity(call, member->loc(), 2, 3))
      return call;
    positions(0);
    return made(Builtin::BytesCopyWithin, type);
  }
  if (name == "with") {
    if (!checkArity(call, member->loc(), 2, 2))
      return call;
    call->args[0] = checkPosition(call->args[0]);
    call->args[1] = checkArg(call->args[1], element);
    checkFits(call->args[1], type, what);
    call->args[1] = convert(call->args[1], Type::doubleTy());
    return made(Builtin::BytesWith, copy);
  }
  if (name == "keys" || name == "values" || name == "entries") {
    // Over a copy of the elements, as an array: the same values, unless the
    // loop changes the typed array meanwhile.
    if (!checkArity(call, member->loc(), 0, 0))
      return call;
    auto *arrayName = ctx_.make<NameExpr>(member->loc(), "Array");
    auto *from = ctx_.make<CallExpr>(member->loc(),
                                     ctx_.make<MemberExpr>(member->loc(), arrayName, "from"),
                                     std::vector<Expr *>{member->object});
    from->builtin = Builtin::ArrayFrom;
    from->type = Type::array(element);
    auto *iterated = ctx_.make<MemberExpr>(member->loc(), from, name);
    auto *iterator = ctx_.make<CallExpr>(call->loc(), iterated, std::vector<Expr *>{});
    return checkArrayIterator(iterator, iterated);
  }
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  if (name == "toLocaleString")
    diags_.error(member->loc(), "'toLocaleString' of a typed array is not supported; join the "
                                "elements with 'join()', or format each with toLocaleString");
  else
    diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                    type->name() + "'");
  return call;
}

namespace {

// A method of Buffer or DataView that reads or writes a number: its kind,
// whether it is big-endian (a DataView takes that as an argument), and for
// readUIntLE and the like, which take the count of bytes, `variable`.
struct NumberMethod {
  bool write = false;
  Bytes kind = Bytes::None;
  bool big = false;
  bool variable = false, isSigned = false;
  bool bigint = false;
};

// readUInt16LE, writeInt32BE, readFloatLE, readUIntBE...; with `dataView`,
// getUint16, setFloat64... Returns false for any other name.
bool numberMethod(llvm::StringRef name, bool dataView, NumberMethod &m) {
  llvm::StringRef rest = name;
  if (dataView) {
    if (!rest.consume_front("get") && !(m.write = rest.consume_front("set")))
      return false;
  } else {
    if (!rest.consume_front("read") && !(m.write = rest.consume_front("write")))
      return false;
    if (rest.consume_back("LE"))
      m.big = false;
    else if (rest.consume_back("BE"))
      m.big = true;
    else if (!rest.ends_with("8"))
      return false;
  }
  static const std::pair<llvm::StringLiteral, Bytes> kinds[] = {
      {"Int8", Bytes::Int8},       {"Uint8", Bytes::Uint8},   {"UInt8", Bytes::Uint8},
      {"Int16", Bytes::Int16},     {"Uint16", Bytes::Uint16}, {"UInt16", Bytes::Uint16},
      {"Int32", Bytes::Int32},     {"Uint32", Bytes::Uint32}, {"UInt32", Bytes::Uint32},
      {"Float16", Bytes::Float16}, {"Float32", Bytes::Float32}, {"Float64", Bytes::Float64},
      {"Float", Bytes::Float32},   {"Double", Bytes::Float64}};
  for (const auto &[n, kind] : kinds) {
    if (rest != n)
      continue;
    // DataView says Float32 and Float64, Buffer Float and Double.
    bool floatName = n == "Float" || n == "Double";
    bool dataViewName = n.starts_with("Float") && !floatName;
    if ((dataView && (floatName || n.starts_with("UInt"))) || (!dataView && dataViewName))
      return false;
    m.kind = kind;
    return true;
  }
  if (rest.starts_with("BigInt64") || rest.starts_with("BigUint64") ||
      rest.starts_with("BigUInt64")) {
    m.bigint = true;
    return true;
  }
  if (!dataView && (rest == "Int" || rest == "UInt" || rest == "Uint")) {
    m.variable = true;
    m.isSigned = rest == "Int";
    return true;
  }
  return false;
}

// "a Uint16", as a value written goes into one.
std::string numberWhat(Bytes kind) {
  switch (kind) {
  case Bytes::Int8:
    return "an Int8";
  case Bytes::Uint8:
    return "a Uint8";
  case Bytes::Int16:
    return "an Int16";
  case Bytes::Uint16:
    return "a Uint16";
  case Bytes::Int32:
    return "an Int32";
  default:
    return "a Uint32";
  }
}

} // namespace

Expr *Sema::checkBufferMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  const Type *uint8 = Type::bytesType(Bytes::Uint8);
  llvm::StringRef name = member->name;
  auto made = [&](Builtin builtin, const Type *result) {
    call->builtin = builtin;
    call->type = result;
    return call;
  };
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  auto intLit = [&](int64_t value) {
    auto *lit = ctx_.make<IntLitExpr>(call->loc(), uint64_t(value));
    lit->type = Type::intTy();
    return lit;
  };
  auto boolLit = [&](bool value) {
    auto *lit = ctx_.make<BoolLitExpr>(call->loc(), value);
    lit->type = Type::boolTy();
    return lit;
  };
  auto bytesArg = [&](Expr *arg) { return checkArg(arg, uint8); };
  // A Uint8Array, a string or a number, which indexOf and fill take.
  auto valueArg = [&](Expr *arg) -> Expr * {
    arg = checkExpr(arg, nullptr);
    const Type *t = arg->type;
    if (t->isError() || t->isString())
      return arg;
    if (t->isInteger())
      return convert(arg, Type::intTy() == t ? t : Type::intTy());
    if (t->isTypedArray() && isImplicitlyConvertible(t, uint8))
      return convert(arg, uint8);
    diags_.error(arg->loc(), "'" + name + "' takes a number, a string or a Uint8Array, found '" +
                                 t->name() + "'");
    arg->type = Type::error();
    return arg;
  };
  auto utf8Lit = [&] { return intLit(Utf8); };

  if (name == "slice") {
    diags_.error(member->loc(), "'slice' of a Buffer is not supported: in node it shares the "
                                "memory, unlike 'slice' of a Uint8Array, and is deprecated; use "
                                "'subarray(start, end)' for the same memory, or "
                                "'Buffer.from(b.subarray(start, end))' for a copy");
    checkArgsLoosely();
    return call;
  }
  if (name == "toString" || name == "toLocaleString") {
    if (!checkArity(call, member->loc(), 0, 3))
      return call;
    std::vector<Expr *> args{call->args.empty() ? utf8Lit() : checkBufferEncoding(call->args[0])};
    for (size_t i = 1; i < call->args.size(); ++i)
      args.push_back(checkPosition(call->args[i]));
    call->args = std::move(args);
    return made(Builtin::BufferToString, Type::string());
  }
  if (name == "toJSON") {
    if (!checkArity(call, member->loc(), 0, 0))
      return call;
    return made(Builtin::BufferToJSON,
                Type::object({{"type", Type::string(), false},
                              {"data", Type::array(Type::intTy()), false}}));
  }
  if (name == "equals") {
    if (!checkArity(call, member->loc(), 1, 1))
      return call;
    call->args[0] = bytesArg(call->args[0]);
    return made(Builtin::BufferEquals, Type::boolTy());
  }
  if (name == "compare") {
    if (!checkArity(call, member->loc(), 1, 5))
      return call;
    call->args[0] = bytesArg(call->args[0]);
    for (size_t i = 1; i < call->args.size(); ++i)
      call->args[i] = checkPosition(call->args[i]);
    return made(Builtin::BufferCompareTo, Type::intTy());
  }
  if (name == "copy") {
    if (!checkArity(call, member->loc(), 1, 4))
      return call;
    call->args[0] = bytesArg(call->args[0]);
    for (size_t i = 1; i < call->args.size(); ++i)
      call->args[i] = checkPosition(call->args[i]);
    return made(Builtin::BufferCopy, Type::intTy());
  }
  if (name == "write") {
    // write(string, offset?, length?, encoding?), where the encoding may come
    // in place of the offset or the length, as node reads it.
    if (!checkArity(call, member->loc(), 1, 4))
      return call;
    Expr *text = checkArg(call->args[0], Type::string());
    Expr *encoding = nullptr;
    std::vector<Expr *> positions;
    for (size_t i = 1; i < call->args.size(); ++i) {
      Expr *arg = call->args[i];
      if (literalText(arg) || i + 1 == call->args.size()) {
        Expr *checked = checkExpr(arg);
        if (checked->type->isString()) {
          encoding = checkBufferEncoding(checked);
          if (i + 1 != call->args.size())
            diags_.error(call->args[i + 1]->loc(), "the encoding comes last in 'write'");
          break;
        }
        positions.push_back(checkPosition(checked));
        continue;
      }
      positions.push_back(checkPosition(arg));
    }
    std::vector<Expr *> args{text, encoding ? encoding : utf8Lit()};
    args.insert(args.end(), positions.begin(), positions.end());
    call->args = std::move(args);
    return made(Builtin::BufferWrite, Type::intTy());
  }
  if (name == "fill" || name == "indexOf" || name == "lastIndexOf" || name == "includes") {
    // fill(value, offset?, end?, encoding?) and indexOf(value, byteOffset?,
    // encoding?), where an encoding may come in place of a position.
    bool fill = name == "fill";
    if (!checkArity(call, member->loc(), 1, fill ? 4 : 3))
      return call;
    Expr *value = valueArg(call->args[0]);
    if (value->type->isError())
      return call;
    if (fill && value->type->isInt())
      checkFits(value, type, "a byte");
    Expr *encoding = nullptr;
    std::vector<Expr *> positions;
    for (size_t i = 1; i < call->args.size(); ++i) {
      Expr *checked = checkExpr(call->args[i]);
      if (checked->type->isString()) {
        encoding = checkBufferEncoding(checked);
        if (i + 1 != call->args.size())
          diags_.error(call->args[i + 1]->loc(), "the encoding comes last in '" + name + "'");
        break;
      }
      positions.push_back(checkPosition(checked));
    }
    if (encoding && !value->type->isString())
      diags_.error(encoding->loc(), "an encoding goes only with a string value");
    std::vector<Expr *> args;
    if (!fill)
      args.push_back(intLit(name == "indexOf" ? 0 : name == "lastIndexOf" ? 1 : 2));
    args.push_back(value);
    args.push_back(encoding ? encoding : utf8Lit());
    args.insert(args.end(), positions.begin(), positions.end());
    call->args = std::move(args);
    if (fill)
      return made(Builtin::BufferFill, type);
    return made(Builtin::BufferIndexOf, name == "includes" ? Type::boolTy() : Type::intTy());
  }
  if (name == "swap16" || name == "swap32" || name == "swap64") {
    if (!checkArity(call, member->loc(), 0, 0))
      return call;
    call->args = {intLit(name == "swap16" ? 2 : name == "swap32" ? 4 : 8)};
    return made(Builtin::BufferSwap, type);
  }
  NumberMethod m;
  if (!numberMethod(name, /*dataView=*/false, m))
    return nullptr; // the methods of Uint8Array
  if (m.bigint) {
    reportBigInt(member->loc(), "'" + name.str() + "'");
    checkArgsLoosely();
    return call;
  }
  Bytes kind = m.kind;
  if (m.variable) {
    // readUIntLE(offset, byteLength) and writeUIntLE(value, offset,
    // byteLength): up to 6 bytes, a long.
    if (!checkArity(call, member->loc(), m.write ? 3 : 2, m.write ? 3 : 2))
      return call;
    std::vector<Expr *> args{boolLit(m.isSigned), boolLit(m.big)};
    size_t at = 0;
    if (m.write) {
      Expr *value = checkArg(call->args[0], Type::longTy());
      args.push_back(value);
      at = 1;
    }
    Expr *offset = checkPosition(call->args[at]);
    Expr *count = checkPosition(call->args[at + 1]);
    args.push_back(offset);
    args.push_back(count);
    if (m.write) {
      std::optional<int64_t> n = intConstant(count);
      if (!n || *n < 1 || *n > 6) {
        diags_.error(count->loc(), "the byte count of '" + name +
                                       "' must be written as a number from 1 to 6, so that the "
                                       "value can be shown to fit");
        return call;
      }
      int64_t bits = *n * 8;
      int64_t lo = m.isSigned ? -(int64_t(1) << (bits - 1)) : 0;
      int64_t hi = m.isSigned ? (int64_t(1) << (bits - 1)) - 1 : (int64_t(1) << bits) - 1;
      if (!args[2]->type->isError() && !provesFits(args[2], lo, hi))
        diags_.error(args[2]->loc(), "cannot prove that the value fits in " +
                                         std::to_string(*n) + " bytes (" + rangeText(lo, hi) +
                                         "), and node would throw a RangeError; check it first");
    }
    call->args = std::move(args);
    call->mayBeOutOfRange = true;
    return made(m.write ? Builtin::BufferWriteVar : Builtin::BufferReadVar,
                m.write ? Type::intTy() : Type::longTy());
  }
  const Type *element = Type::bytesType(kind)->elementType();
  int64_t size = Type::bytesType(kind)->elementSize();
  if (!checkArity(call, member->loc(), m.write ? 1 : 0, m.write ? 2 : 1))
    return call;
  std::vector<Expr *> args{intLit(int64_t(kind)), boolLit(m.big)};
  size_t at = 0;
  if (m.write) {
    Expr *value = checkArg(call->args[0], element);
    checkFits(value, Type::bytesType(kind), numberWhat(kind));
    args.push_back(value);
    at = 1;
  }
  Expr *offset = call->args.size() > at ? checkPosition(call->args[at]) : nullptr;
  if (offset)
    args.push_back(offset);
  call->mayBeOutOfRange = !provesWithin(member->object, offset, size);
  call->args = std::move(args);
  return made(m.write ? Builtin::BufferWriteNumber : Builtin::BufferRead,
              m.write ? Type::intTy() : element);
}

Expr *Sema::checkDataViewMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  NumberMethod m;
  if (!numberMethod(name, /*dataView=*/true, m)) {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    diags_.error(member->loc(), "property '" + name + "' does not exist on type 'DataView'");
    return call;
  }
  if (m.bigint) {
    reportBigInt(member->loc(), "'" + name.str() + "'");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  const Type *kindType = Type::bytesType(m.kind);
  const Type *element = kindType->elementType();
  bool oneByte = kindType->elementSize() == 1;
  unsigned min = m.write ? 2 : 1, max = min + (oneByte ? 0 : 1);
  if (!checkArity(call, member->loc(), min, max))
    return call;
  auto *kind = ctx_.make<IntLitExpr>(call->loc(), uint64_t(m.kind));
  kind->type = Type::intTy();
  std::vector<Expr *> args{kind, checkPosition(call->args[0])};
  if (m.write) {
    Expr *value = checkArg(call->args[1], element);
    checkFits(value, kindType, numberWhat(m.kind));
    args.push_back(value);
  }
  if (call->args.size() > min)
    args.push_back(checkArg(call->args[min], Type::boolTy()));
  call->mayBeOutOfRange = !provesWithin(member->object, args[1], kindType->elementSize());
  call->args = std::move(args);
  call->builtin = m.write ? Builtin::DataViewSet : Builtin::DataViewGet;
  call->type = m.write ? Type::voidTy() : element;
  return call;
}

Expr *Sema::checkTextMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  if (type->bytes() == Bytes::TextEncoder && name == "encode") {
    if (!checkArity(call, member->loc(), 0, 1))
      return call;
    if (!call->args.empty())
      call->args[0] = checkArg(call->args[0], Type::string());
    call->builtin = Builtin::TextEncode;
    call->type = Type::bytesType(Bytes::Uint8);
    return call;
  }
  if (type->bytes() == Bytes::TextEncoder && name == "encodeInto") {
    if (!checkArity(call, member->loc(), 2, 2))
      return call;
    call->args[0] = checkArg(call->args[0], Type::string());
    call->args[1] = checkArg(call->args[1], Type::bytesType(Bytes::Uint8));
    call->builtin = Builtin::TextEncodeInto;
    call->type =
        Type::object({{"read", Type::intTy(), false}, {"written", Type::intTy(), false}});
    return call;
  }
  if (type->bytes() == Bytes::TextDecoder && name == "decode") {
    if (!checkArity(call, member->loc(), 0, 2))
      return call;
    if (call->args.size() == 2) {
      call->args[1] = checkExpr(call->args[1]);
      diags_.error(call->args[1]->loc(), "the options of 'decode' ({ stream: true }) are not "
                                         "supported yet; decode the bytes whole");
      return call;
    }
    if (!call->args.empty()) {
      Expr *input = call->args[0] = checkExpr(call->args[0]);
      const Type *t = input->type;
      if (!t->isError() && !t->isByteView() && t->bytes() != Bytes::ArrayBuffer) {
        diags_.error(input->loc(), "'decode' takes a typed array, a DataView or an ArrayBuffer, "
                                   "found '" + t->name() + "'");
        return call;
      }
    }
    call->builtin = Builtin::TextDecode;
    call->type = Type::string();
    return call;
  }
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  diags_.error(member->loc(), "property '" + name + "' does not exist on type '" + type->name() +
                                  "'");
  return call;
}

//===----------------------------------------------------------------------===//
// Static functions
//===----------------------------------------------------------------------===//

Expr *Sema::checkBytesStatic(CallExpr *call, MemberExpr *member) {
  auto *object = dyn_cast<NameExpr>(member->object);
  if (!object || !isGlobalName(object, object->name))
    return nullptr;
  if (object->name == "BigInt64Array" || object->name == "BigUint64Array") {
    reportBigInt(object->loc(), "'" + object->name.str() + "'");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    call->type = Type::error();
    return call;
  }
  const Type *type = bytesTypeNamed(object->name);
  if (!type)
    return nullptr;
  call->type = Type::error();
  if (rejectSpread(call))
    return call;
  llvm::StringRef name = member->name;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  auto made = [&](Builtin builtin, const Type *result) {
    call->builtin = builtin;
    call->type = result;
    return call;
  };
  if (type->bytes() == Bytes::ArrayBuffer && name == "isView") {
    if (!checkArity(call, member->loc(), 1, 1))
      return call;
    call->args[0] = checkExpr(call->args[0]);
    return made(Builtin::ArrayBufferIsView, Type::boolTy());
  }
  if (type->isBuffer() && name != "of")
    return checkBufferStatic(call, member);
  if (!type->isTypedArray()) {
    checkArgsLoosely();
    diags_.error(member->loc(), "'" + object->name + "." + name + "' does not exist");
    return call;
  }
  if (name == "of") {
    auto *lit = ctx_.make<ArrayLitExpr>(call->loc());
    for (Expr *arg : call->args)
      lit->elements.push_back({arg, false});
    return checkBytesValues(call, lit, type);
  }
  if (name != "from") {
    checkArgsLoosely();
    diags_.error(member->loc(), "'" + object->name + "." + name + "' is not supported; of " +
                                    object->name + " there are 'from' and 'of'");
    return call;
  }
  if (!checkArity(call, member->loc(), 1, 2))
    return call;
  Expr *&source = call->args[0];
  if (call->args.size() == 1) {
    if (auto *lit = dyn_cast<ArrayLitExpr>(source))
      return checkBytesValues(call, lit, type);
    markFused(source);
    source = checkExpr(source);
    if (!checkBytesSource(source, type))
      return call;
    return made(source->type->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray,
                type);
  }
  return checkBytesFromWith(call, type, "'" + object->name.str() + ".from'");
}

Expr *Sema::checkBytesFromWith(CallExpr *call, const Type *type, const std::string &what) {
  // `Uint8Array.from(xs, f)`: each value through f, whose result must fit;
  // made as Array.from(xs, f) is, and then the typed array of that.
  Expr *&source = call->args[0];
  markFused(source);
  source = checkExpr(source);
  const Type *from = source->type;
  if (from->isError())
    return call;
  const Type *element = iteratedType(from);
  if (!element || from->isString()) {
    diags_.error(source->loc(), what + " takes an array, a typed array or an iterator, found '" +
                                    from->name() + "'");
    return call;
  }
  if (from->isIterator())
    checkSendsNothing(from, source->loc(), what);
  Signature sig;
  sig.pad = false;
  sig.params = {element, Type::intTy()};
  sig.ret = type->elementType();
  if (auto *fe = dyn_cast<FunctionExpr>(call->args[1])) {
    ByteCallback info;
    if (!from->elementRange(info.lo, info.hi)) {
      info.lo = kIntMin;
      info.hi = kIntMax;
    }
    info.target = type;
    byteCallbacks_[fe->fn] = info;
  }
  bool arrow = isa<FunctionExpr>(call->args[1]);
  call->args[1] = checkFunctionArg(call->args[1], sig, 1, what);
  if (!call->args[1]->type->isFunction())
    return call;
  int64_t lo, hi;
  if (!arrow && type->elementRange(lo, hi) && type->bytes() != Bytes::Uint8Clamped &&
      type->bytes() != Bytes::Int32) {
    diags_.error(call->args[1]->loc(),
                 "cannot prove that what the function returns fits in an element of '" +
                     type->name() + "' (" + rangeText(lo, hi) +
                     "), and JavaScript would cut it without a word; pass an arrow that cuts it "
                     "explicitly, e.g. 'x => " + wrapHint(type->bytes(), "f(x)") + "'");
    return call;
  }
  calledFunction(); // a generator or the callback may change anything
  auto *arrayFrom = ctx_.make<CallExpr>(
      call->loc(),
      ctx_.make<MemberExpr>(call->loc(), ctx_.make<NameExpr>(call->loc(), "Array"), "from"),
      std::vector<Expr *>{source, call->args[1]});
  arrayFrom->builtin = Builtin::ArrayFrom;
  arrayFrom->type = Type::array(type->elementType());
  call->args = {arrayFrom};
  call->builtin = Builtin::BytesFromArray;
  call->type = type;
  return call;
}

Expr *Sema::checkBufferStatic(CallExpr *call, MemberExpr *member) {
  const Type *buffer = Type::bytesType(Bytes::Buffer);
  const Type *uint8 = Type::bytesType(Bytes::Uint8);
  llvm::StringRef name = member->name;
  auto made = [&](Builtin builtin, const Type *result) {
    call->builtin = builtin;
    call->type = result;
    return call;
  };
  auto boolLit = [&](bool value) {
    auto *lit = ctx_.make<BoolLitExpr>(call->loc(), value);
    lit->type = Type::boolTy();
    return lit;
  };
  auto utf8Lit = [&] {
    auto *lit = ctx_.make<IntLitExpr>(call->loc(), Utf8);
    lit->type = Type::intTy();
    return lit;
  };
  if (name == "from") {
    if (!checkArity(call, member->loc(), 1, 3))
      return call;
    Expr *&source = call->args[0];
    if (auto *lit = dyn_cast<ArrayLitExpr>(source); lit && call->args.size() == 1)
      return checkBytesValues(call, lit, buffer);
    markFused(source);
    source = checkExpr(source);
    const Type *from = source->type;
    if (from->isError())
      return call;
    if (from->isString()) {
      if (call->args.size() > 2) {
        checkArity(call, member->loc(), 1, 2);
        return call;
      }
      call->args.resize(2, nullptr);
      call->args[1] = call->args[1] ? checkBufferEncoding(call->args[1]) : utf8Lit();
      return made(Builtin::BufferFromString, buffer);
    }
    if (from->bytes() == Bytes::ArrayBuffer) {
      for (size_t i = 1; i < call->args.size(); ++i)
        call->args[i] = checkPosition(call->args[i]);
      return made(Builtin::BytesView, buffer);
    }
    if (call->args.size() > 1) {
      diags_.error(call->args[1]->loc(), "'Buffer.from' takes an offset and a length only with "
                                         "an ArrayBuffer, found '" + from->name() + "'");
      return call;
    }
    if (!checkBytesSource(source, buffer))
      return call;
    return made(from->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray, buffer);
  }
  if (name == "alloc") {
    if (!checkArity(call, member->loc(), 1, 3))
      return call;
    call->args[0] = checkPosition(call->args[0]);
    if (std::optional<Linear> n = linear(call->args[0]); n && n->term.isZero() && n->offset < 0)
      diags_.error(call->args[0]->loc(), "the size of a Buffer cannot be negative");
    if (call->args.size() == 1) {
      call->args.push_back(boolLit(true));
      return made(Builtin::BytesNew, buffer);
    }
    Expr *fill = checkExpr(call->args[1]);
    const Type *t = fill->type;
    if (t->isError())
      return call;
    if (t->isInteger()) {
      fill = convert(fill, Type::intTy() == t ? t : Type::intTy());
      checkFits(fill, buffer, "a byte");
    } else if (t->isTypedArray() && isImplicitlyConvertible(t, uint8)) {
      fill = convert(fill, uint8);
    } else if (!t->isString()) {
      diags_.error(fill->loc(), "'Buffer.alloc' fills with a number, a string or a Uint8Array, "
                                "found '" + t->name() + "'");
      return call;
    }
    Expr *encoding = utf8Lit();
    if (call->args.size() == 3) {
      encoding = checkBufferEncoding(call->args[2]);
      if (!t->isString())
        diags_.error(encoding->loc(), "an encoding goes only with a string to fill with");
    }
    call->args = {call->args[0], fill, encoding};
    return made(Builtin::BufferAllocFill, buffer);
  }
  if (name == "allocUnsafe" || name == "allocUnsafeSlow") {
    if (!checkArity(call, member->loc(), 1, 1))
      return call;
    call->args[0] = checkPosition(call->args[0]);
    call->args.push_back(boolLit(false));
    return made(Builtin::BytesNew, buffer);
  }
  if (name == "concat") {
    if (!checkArity(call, member->loc(), 1, 2))
      return call;
    Expr *&list = call->args[0];
    list = checkExpr(list, Type::array(uint8));
    const Type *t = list->type;
    if (t->isError())
      return call;
    if (!t->isArray() || (t->inner() != uint8 && t->inner() != buffer)) {
      diags_.error(list->loc(), "'Buffer.concat' takes an array of Uint8Arrays or Buffers, "
                                "found '" + t->name() + "'");
      return call;
    }
    if (call->args.size() == 2)
      call->args[1] = checkPosition(call->args[1]);
    return made(Builtin::BufferConcat, buffer);
  }
  if (name == "byteLength") {
    if (!checkArity(call, member->loc(), 1, 2))
      return call;
    call->args[0] = checkExpr(call->args[0]);
    const Type *t = call->args[0]->type;
    if (t->isError())
      return call;
    if (t->isString()) {
      call->args.resize(2, nullptr);
      call->args[1] = call->args[1] ? checkBufferEncoding(call->args[1]) : utf8Lit();
    } else if (!t->isByteView() && t->bytes() != Bytes::ArrayBuffer) {
      diags_.error(call->args[0]->loc(), "'Buffer.byteLength' takes a string, a typed array, a "
                                         "DataView or an ArrayBuffer, found '" + t->name() + "'");
      return call;
    }
    return made(Builtin::BufferByteLength, Type::intTy());
  }
  if (name == "compare") {
    if (!checkArity(call, member->loc(), 2, 2))
      return call;
    for (Expr *&arg : call->args)
      arg = checkArg(arg, uint8);
    return made(Builtin::BufferCompare, Type::intTy());
  }
  if (name == "isBuffer") {
    if (!checkArity(call, member->loc(), 1, 1))
      return call;
    call->args[0] = checkExpr(call->args[0]);
    // Of a value that may be a Buffer, but not surely, the check of
    // `instanceof Buffer`, which narrows it too.
    const Type *arg = call->args[0]->type, *base = arg->nonOptional();
    auto mayBe = [&](const Type *t) { return t->isBuffer() || t->bytes() == Bytes::Uint8; };
    llvm::ArrayRef<const Type *> members =
        base->isUnion() ? base->members() : llvm::ArrayRef<const Type *>(base);
    if (!arg->isError() && llvm::any_of(members, mayBe) &&
        (arg->isOptional() || !llvm::all_of(members, [](const Type *t) { return t->isBuffer(); }))) {
      auto *rhs = ctx_.make<NameExpr>(member->loc(), "Buffer");
      rhs->type = Type::bytesType(Bytes::Buffer);
      auto *test = ctx_.make<BinaryExpr>(call->loc(), BinaryOp::InstanceOf, call->args[0], rhs);
      test->type = Type::boolTy();
      return test;
    }
    return made(Builtin::BufferIsBuffer, Type::boolTy());
  }
  if (name == "isEncoding") {
    if (!checkArity(call, member->loc(), 1, 1))
      return call;
    if (std::optional<std::u16string> text = literalText(call->args[0]))
      return boolLit(encodingNamed(utf8(*text)).has_value());
    call->args[0] = checkArg(call->args[0], Type::string());
    return made(Builtin::BufferIsEncoding, Type::boolTy());
  }
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  diags_.error(member->loc(), "'Buffer." + name + "' is not supported; Buffer has from, alloc, "
                                  "allocUnsafe, concat, byteLength, compare, isBuffer, isEncoding "
                                  "and of");
  return call;
}

Expr *Sema::checkEmbedBytes(CallExpr *call, NameExpr *name) {
  call->type = Type::error();
  if (!checkArity(call, name->loc(), 1, 1))
    return call;
  std::string bytes, shown, full;
  if (!readEmbedded(call, "embedBytes", bytes, shown, full))
    return call;
  if (int64_t(bytes.size()) > kMaxLength) {
    diags_.error(call->args[0]->loc(), "'" + shown + "' is larger than the 1 GiB a Uint8Array "
                                                     "may have");
    return call;
  }
  // The bytes, each a code unit of a literal, and the file they came from:
  // one file gives one array, whichever file of the program embeds it.
  llvm::SMLoc loc = call->args[0]->loc();
  std::u16string units(bytes.size(), u'\0');
  for (size_t i = 0; i < bytes.size(); ++i)
    units[i] = char16_t(static_cast<unsigned char>(bytes[i]));
  auto *lit = ctx_.make<StringLitExpr>(loc, std::move(units));
  lit->type = Type::string();
  llvm::SmallVector<llvm::UTF16, 64> path;
  llvm::convertUTF8ToUTF16String(full, path);
  auto *file = ctx_.make<StringLitExpr>(loc, std::u16string(path.begin(), path.end()));
  file->type = Type::string();
  call->args = {lit, file};
  call->builtin = Builtin::EmbedBytes;
  call->type = Type::bytesType(Bytes::Uint8);
  return call;
}

} // namespace tz
