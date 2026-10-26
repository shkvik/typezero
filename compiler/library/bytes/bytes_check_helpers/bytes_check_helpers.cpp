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

bool known(const Range &r) { return r.lo && r.hi; }

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

}

std::optional<int> encodingNamed(llvm::StringRef name) {
  static const std::pair<llvm::StringLiteral, Encoding> names[] = {
      {"utf8", Utf8},    {"utf-8", Utf8},    {"utf16le", Utf16le},     {"utf-16le", Utf16le},
      {"ucs2", Utf16le}, {"ucs-2", Utf16le}, {"latin1", Latin1},       {"binary", Latin1},
      {"ascii", Ascii},  {"base64", Base64}, {"base64url", Base64url}, {"hex", Hex},
  };
  std::string lower = name.lower();
  for (const auto &[n, e] : names) {
    if (n == lower) {
      return int(e);
    }
  }

  return std::nullopt;
}

std::string valueText(const Expr *e) {
  while (auto *conv = dyn_cast<CastExpr>(e)) {
    if (!conv->isImplicit()) {
      break;
    }
    e = conv->operand;
  }
  if (auto *name = dyn_cast<NameExpr>(e)) {
    return name->name.str();
  }
  if (auto *lit = dyn_cast<IntLitExpr>(e)) {
    return std::to_string(lit->value);
  }
  if (auto *bin = dyn_cast<BinaryExpr>(e); bin && !bin->source.empty()) {
    return "(" + bin->source.str() + ")";
  }
  if (auto *index = dyn_cast<IndexExpr>(e)) {
    std::string object = valueText(index->object), position = valueText(index->index);
    if (object != "x" && position != "x") {
      return object + "[" + position + "]";
    }
  }

  return "x";
}

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

Range operate(BinaryOp op, const Range &a, const Range &b, bool isInt) {
  int64_t min = isInt ? kIntMin : std::numeric_limits<int64_t>::min();
  int64_t max = isInt ? kIntMax : std::numeric_limits<int64_t>::max();
  auto within = [&](Range r) -> Range {
    if (!known(r) || *r.lo < min || *r.hi > max) {
      return {};
    }

    return r;
  };
  std::optional<int64_t> count;
  if (b.lo && b.hi && *b.lo == *b.hi) {
    count = *b.lo;
  }
  switch (op) {
  case BinaryOp::BitAnd: {
    Range r;
    for (const Range *side : {&a, &b}) {
      if (side->lo && *side->lo >= 0) {
        r.lo = 0;
        if (side->hi) {
          r.hi = r.hi ? std::min(*r.hi, *side->hi) : *side->hi;
        }
      }
    }
    return r;
  }
  case BinaryOp::BitOr:
  case BinaryOp::BitXor:
    if (known(a) && known(b) && *a.lo >= 0 && *b.lo >= 0) {
      return {op == BinaryOp::BitOr ? std::max(*a.lo, *b.lo) : 0, allOnes(std::max(*a.hi, *b.hi))};
    }
    return {};
  case BinaryOp::Shl:
    if (count && known(a) && *count >= 0 && *count < (isInt ? 32 : 64) && *count < 40 &&
        *a.lo > -(int64_t(1) << 40) && *a.hi < (int64_t(1) << 40)) {
      return within({*a.lo * (int64_t(1) << *count), *a.hi * (int64_t(1) << *count)});
    }
    return {};
  case BinaryOp::Shr: {
    if (!count) {
      return {};
    }
    int64_t n = *count & (isInt ? 31 : 63);
    if (known(a)) {
      return {floorShift(*a.lo, n), floorShift(*a.hi, n)};
    }
    if (isInt) {
      return {floorShift(kIntMin, n), floorShift(kIntMax, n)};
    }
    return {};
  }
  case BinaryOp::UShr: {
    if (!count) {
      return {};
    }
    int64_t n = *count & (isInt ? 31 : 63);
    if (a.lo && *a.lo >= 0 && a.hi) {
      return {*a.lo >> n, *a.hi >> n};
    }
    if (isInt) {
      return {0, int64_t(0xFFFFFFFF) >> n};
    }
    if (n > 0) {
      return {0, std::numeric_limits<int64_t>::max() >> (n - 1)};
    }
    return {};
  }
  case BinaryOp::Rem: {
    if (!count || *count == 0) {
      return {};
    }
    int64_t m = (*count < 0 ? -*count : *count) - 1;
    if (a.lo && *a.lo >= 0) {
      return {0, a.hi ? std::min(m, *a.hi) : m};
    }
    if (a.hi && *a.hi <= 0) {
      return {a.lo ? std::max(-m, *a.lo) : -m, 0};
    }
    return {-m, m};
  }
  case BinaryOp::Div:
    if (count && *count > 0 && known(a)) {
      return {*a.lo / *count, *a.hi / *count};
    }
    return {};
  case BinaryOp::Add:
  case BinaryOp::Sub:
  case BinaryOp::Mul: {
    if (!known(a) || !known(b)) {
      return {};
    }
    int64_t xs[2] = {*a.lo, *a.hi}, ys[2] = {*b.lo, *b.hi};
    int64_t lo = std::numeric_limits<int64_t>::max(), hi = std::numeric_limits<int64_t>::min();
    for (int64_t x : xs) {
      for (int64_t y : ys) {
        int64_t v;
        bool overflow = op == BinaryOp::Add   ? llvm::AddOverflow(x, y, v)
                        : op == BinaryOp::Sub ? llvm::SubOverflow(x, y, v)
                                              : llvm::MulOverflow(x, y, v);
        if (overflow) {
          return {};
        }
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
    }
    return within({lo, hi});
  }
  default:
    return {};
  }
}

bool numberMethod(llvm::StringRef name, bool dataView, NumberMethod &m) {
  llvm::StringRef rest = name;
  if (dataView) {
    if (!rest.consume_front("get") && !(m.write = rest.consume_front("set"))) {
      return false;
    }
  } else {
    if (!rest.consume_front("read") && !(m.write = rest.consume_front("write"))) {
      return false;
    }
    if (rest.consume_back("LE")) {
      m.big = false;
    } else if (rest.consume_back("BE")) {
      m.big = true;
    } else if (!rest.ends_with("8")) {
      return false;
    }
  }
  static const std::pair<llvm::StringLiteral, Bytes> kinds[] = {
      {"Int8", Bytes::Int8},       {"Uint8", Bytes::Uint8},     {"UInt8", Bytes::Uint8},
      {"Int16", Bytes::Int16},     {"Uint16", Bytes::Uint16},   {"UInt16", Bytes::Uint16},
      {"Int32", Bytes::Int32},     {"Uint32", Bytes::Uint32},   {"UInt32", Bytes::Uint32},
      {"Float16", Bytes::Float16}, {"Float32", Bytes::Float32}, {"Float64", Bytes::Float64},
      {"Float", Bytes::Float32},   {"Double", Bytes::Float64}};
  for (const auto &[n, kind] : kinds) {
    if (rest != n) {
      continue;
    }
    bool floatName = n == "Float" || n == "Double";
    bool dataViewName = n.starts_with("Float") && !floatName;
    if ((dataView && (floatName || n.starts_with("UInt"))) || (!dataView && dataViewName)) {
      return false;
    }
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

}
