#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
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

Range BytesCheckHelpers::operateWide(BinaryOp op, const Range &a, const Range &b, bool isInt) {
  std::optional<int64_t> count = BytesCheckHelpers::exactValue(b);
  switch (op) {
    case BinaryOp::BitAnd:
      return BytesCheckHelpers::bitAndRange(a, b);
    case BinaryOp::BitOr:
    case BinaryOp::BitXor:
      return BytesCheckHelpers::bitOrRange(op, a, b);
    case BinaryOp::Shl:
      return BytesCheckHelpers::shiftLeftRange(a, count, isInt);
    case BinaryOp::Shr:
      return BytesCheckHelpers::shiftRightRange(a, count, isInt);
    case BinaryOp::UShr:
      return BytesCheckHelpers::unsignedShiftRange(a, count, isInt);
    case BinaryOp::Rem:
      return BytesCheckHelpers::remainderRange(a, count);
    case BinaryOp::Div:
      return BytesCheckHelpers::divisionRange(a, count);
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Mul:
      return BytesCheckHelpers::arithmeticRange(op, a, b);
    case BinaryOp::Pow:
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::LogAnd:
    case BinaryOp::LogOr:
    case BinaryOp::Nullish:
    case BinaryOp::In:
    case BinaryOp::InstanceOf:
      return {};
  }

  return {};
}

std::optional<int64_t> BytesCheckHelpers::exactValue(const Range &r) {
  if (!r.lo.has_value() || !r.hi.has_value() || *r.lo != *r.hi) {
    return std::nullopt;
  }

  return *r.lo;
}

Range BytesCheckHelpers::withinBounds(const Range &r, bool isInt) {
  int64_t min = isInt ? kInt32Min : kIntMin;
  int64_t max = isInt ? kInt32Max : kIntMax;
  if (!BytesCheckHelpers::known(r) || *r.lo < min || *r.hi > max) {
    return {};
  }

  return r;
}

Range BytesCheckHelpers::bitAndRange(const Range &a, const Range &b) {
  Range r;
  for (const Range *side : {&a, &b}) {
    if (!side->lo.has_value() || *side->lo < 0) {
      continue;
    }
    r.lo = 0;
    if (side->hi.has_value()) {
      r.hi = r.hi.has_value() ? std::min(*r.hi, *side->hi) : *side->hi;
    }
  }

  return r;
}

Range BytesCheckHelpers::bitOrRange(BinaryOp op, const Range &a, const Range &b) {
  bool isNatural =
      BytesCheckHelpers::known(a) && BytesCheckHelpers::known(b) && *a.lo >= 0 && *b.lo >= 0;
  if (!isNatural) {
    return {};
  }
  int64_t lo = op == BinaryOp::BitOr ? std::max(*a.lo, *b.lo) : 0;

  return {lo, BytesCheckHelpers::allOnes(std::max(*a.hi, *b.hi))};
}

Range BytesCheckHelpers::shiftLeftRange(const Range &a, std::optional<int64_t> count, bool isInt) {
  bool isSmall = count.has_value() && BytesCheckHelpers::known(a) && *count >= 0 &&
                 *count < (isInt ? 32 : 64) && *count < 40 && *a.lo > -(int64_t(1) << 40) &&
                 *a.hi < (int64_t(1) << 40);
  if (!isSmall) {
    return {};
  }
  int64_t factor = int64_t(1) << *count;

  return BytesCheckHelpers::withinBounds({*a.lo * factor, *a.hi * factor}, isInt);
}

Range BytesCheckHelpers::shiftRightRange(const Range &a, std::optional<int64_t> count, bool isInt) {
  if (!count.has_value()) {
    return {};
  }
  int64_t n = *count & (isInt ? 31 : 63);
  if (BytesCheckHelpers::known(a)) {
    return {BytesCheckHelpers::floorShift(*a.lo, n), BytesCheckHelpers::floorShift(*a.hi, n)};
  }
  if (isInt) {
    return {BytesCheckHelpers::floorShift(kInt32Min, n),
            BytesCheckHelpers::floorShift(kInt32Max, n)};
  }

  return {};
}

Range BytesCheckHelpers::unsignedShiftRange(const Range &a, std::optional<int64_t> count,
                                            bool isInt) {
  if (!count.has_value()) {
    return {};
  }
  int64_t n = *count & (isInt ? 31 : 63);
  if (a.lo.has_value() && *a.lo >= 0 && a.hi.has_value()) {
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

Range BytesCheckHelpers::remainderRange(const Range &a, std::optional<int64_t> count) {
  if (!count.has_value() || *count == 0) {
    return {};
  }
  int64_t m = (*count < 0 ? -*count : *count) - 1;
  if (a.lo.has_value() && *a.lo >= 0) {
    return {0, a.hi.has_value() ? std::min(m, *a.hi) : m};
  }
  if (a.hi.has_value() && *a.hi <= 0) {
    return {a.lo.has_value() ? std::max(-m, *a.lo) : -m, 0};
  }

  return {-m, m};
}

Range BytesCheckHelpers::divisionRange(const Range &a, std::optional<int64_t> count) {
  if (!count.has_value() || *count <= 0 || !BytesCheckHelpers::known(a)) {
    return {};
  }

  return {*a.lo / *count, *a.hi / *count};
}

Range BytesCheckHelpers::arithmeticRange(BinaryOp op, const Range &a, const Range &b) {
  bool isExact = BytesCheckHelpers::known(a) && BytesCheckHelpers::known(b);
  if (op != BinaryOp::Mul && !isExact) {
    return BytesCheckHelpers::openSumRange(op, a, b);
  }
  if (!isExact) {
    return {};
  }

  return BytesCheckHelpers::cornersRange(op, a, b);
}

Range BytesCheckHelpers::openSumRange(BinaryOp op, const Range &a, const Range &b) {
  bool isAdd = op == BinaryOp::Add;
  const std::optional<int64_t> &low = isAdd ? b.lo : b.hi;
  const std::optional<int64_t> &high = isAdd ? b.hi : b.lo;
  Range r;
  int64_t v = 0;
  if (a.lo.has_value() && low.has_value() &&
      !(isAdd ? llvm::AddOverflow(*a.lo, *low, v) : llvm::SubOverflow(*a.lo, *low, v))) {
    r.lo = v;
  }
  if (a.hi.has_value() && high.has_value() &&
      !(isAdd ? llvm::AddOverflow(*a.hi, *high, v) : llvm::SubOverflow(*a.hi, *high, v))) {
    r.hi = v;
  }

  return r;
}

Range BytesCheckHelpers::cornersRange(BinaryOp op, const Range &a, const Range &b) {
  int64_t xs[2] = {*a.lo, *a.hi};
  int64_t ys[2] = {*b.lo, *b.hi};
  int64_t lo = std::numeric_limits<int64_t>::max();
  int64_t hi = std::numeric_limits<int64_t>::min();
  for (int64_t x : xs) {
    for (int64_t y : ys) {
      int64_t v = 0;
      bool isOverflow = op == BinaryOp::Add   ? llvm::AddOverflow(x, y, v)
                        : op == BinaryOp::Sub ? llvm::SubOverflow(x, y, v)
                                              : llvm::MulOverflow(x, y, v);
      if (isOverflow) {
        return {};
      }
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  }

  return Range{lo, hi};
}

bool BytesCheckHelpers::known(const Range &r) {
  return r.lo && r.hi;
}

int64_t BytesCheckHelpers::allOnes(int64_t v) {
  int64_t m = 0;
  while (m < v) {
    m = m * 2 + 1;
  }

  return m;
}

int64_t BytesCheckHelpers::floorShift(int64_t v, int64_t n) {
  return v >= 0 ? v >> n : -((-v + (int64_t(1) << n) - 1) >> n);
}

std::optional<int> BytesCheckHelpers::encodingNamed(llvm::StringRef name) {
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

std::string BytesCheckHelpers::valueText(const Expr *e) {
  std::vector<std::string> answers;
  for (const Expr *node : ChildrenFirst::order(e, BytesCheckHelpers::valueParts)) {
    std::string text = BytesCheckHelpers::nodeValueText(node, answers);
    if (text == "x") {
      return text;
    }
    answers.push_back(std::move(text));
  }

  return answers.back();
}

NodeChildren<const Expr *> BytesCheckHelpers::valueParts(const Expr *e) {
  if (auto *conv = dyn_cast<CastExpr>(e); conv && conv->isImplicit()) {
    return {conv->operand};
  }
  if (auto *index = dyn_cast<IndexExpr>(e)) {
    return {index->object, index->index};
  }

  return {};
}

std::string BytesCheckHelpers::nodeValueText(const Expr *e, std::vector<std::string> &answers) {
  if (auto *conv = dyn_cast<CastExpr>(e); conv && conv->isImplicit()) {
    return ChildrenFirst::takeAnswer(answers);
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
  if (llvm::isa<IndexExpr>(e)) {
    std::string position = ChildrenFirst::takeAnswer(answers);
    std::string object = ChildrenFirst::takeAnswer(answers);
    return object + "[" + position + "]";
  }

  return "x";
}

std::string BytesCheckHelpers::wrapHint(Bytes kind, const std::string &value) {
  switch (kind) {
    case Bytes::Int8:
      return "((" + value + " & 0xff) ^ 0x80) - 0x80";
    case Bytes::Int16:
      return "((" + value + " & 0xffff) ^ 0x8000) - 0x8000";
    case Bytes::Uint16:
      return value + " & 0xffff";
    case Bytes::Uint32:
      return value + " & 0xffffffff";
    case Bytes::Int32:
      return "((" + value + " & 0xffffffff) ^ 0x80000000) - 0x80000000";
    default:
      return value + " & 0xff";
  }
}

Range BytesCheckHelpers::operate(BinaryOp op, const Range &a, const Range &b, bool isInt) {
  Range r = BytesCheckHelpers::operateWide(op, a, b, isInt);
  bool isBitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor ||
                   op == BinaryOp::Shl || op == BinaryOp::Shr;
  if (isInt && isBitwise && !BytesCheckHelpers::known(r)) {
    return {kInt32Min, kInt32Max};
  }

  return r;
}

bool BytesCheckHelpers::numberMethod(llvm::StringRef name, bool dataView, NumberMethod &m) {
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
    bool isFloatName = n == "Float" || n == "Double";
    bool isDataViewName = n.starts_with("Float") && !isFloatName;
    if ((dataView && (isFloatName || n.starts_with("UInt"))) || (!dataView && isDataViewName)) {
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

std::string BytesCheckHelpers::numberWhat(Bytes kind) {
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
