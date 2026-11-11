#pragma once

#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MathExtras.h"
#include <limits>

namespace tz {

using Bytes = Type::Bytes;

using Range = IntegerRange;

inline constexpr int64_t kInt32Min = std::numeric_limits<int32_t>::min();

inline constexpr int64_t kInt32Max = std::numeric_limits<int32_t>::max();

inline constexpr int64_t kIntMin = std::numeric_limits<int64_t>::min();

inline constexpr int64_t kIntMax = std::numeric_limits<int64_t>::max();

enum Encoding { Utf8, Utf16le, Latin1, Ascii, Base64, Base64url, Hex };

struct NumberMethod {
  bool write = false;
  Bytes kind = Bytes::None;
  bool big = false;
  bool variable = false, isSigned = false;
  bool bigint = false;
};

class BytesCheckHelpers {
public:
  static std::optional<int> encodingNamed(llvm::StringRef name);
  static std::string valueText(const Expr *e);
  static std::string wrapHint(Bytes kind, const std::string &value);
  static Range operate(BinaryOp op, const Range &a, const Range &b, bool isInt);
  static bool numberMethod(llvm::StringRef name, bool dataView, NumberMethod &m);
  static std::string numberWhat(Bytes kind);

private:
  static NodeChildren<const Expr *> valueParts(const Expr *e);
  static std::string nodeValueText(const Expr *e, std::vector<std::string> &answers);
  static Range operateWide(BinaryOp op, const Range &a, const Range &b, bool isInt);
  static std::optional<int64_t> exactValue(const Range &r);
  static Range withinBounds(const Range &r, bool isInt);
  static Range bitAndRange(const Range &a, const Range &b);
  static Range bitOrRange(BinaryOp op, const Range &a, const Range &b);
  static Range shiftLeftRange(const Range &a, std::optional<int64_t> count, bool isInt);
  static Range shiftRightRange(const Range &a, std::optional<int64_t> count, bool isInt);
  static Range unsignedShiftRange(const Range &a, std::optional<int64_t> count, bool isInt);
  static Range remainderRange(const Range &a, std::optional<int64_t> count);
  static Range divisionRange(const Range &a, std::optional<int64_t> count);
  static Range arithmeticRange(BinaryOp op, const Range &a, const Range &b);
  static Range openSumRange(BinaryOp op, const Range &a, const Range &b);
  static Range cornersRange(BinaryOp op, const Range &a, const Range &b);
  static bool known(const Range &r);
  static int64_t allOnes(int64_t v);
  static int64_t floorShift(int64_t v, int64_t n);
};

}
