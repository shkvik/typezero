#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
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
  static Range operateWide(BinaryOp op, const Range &a, const Range &b, bool isInt);
  static bool known(const Range &r);
  static int64_t allOnes(int64_t v);
  static int64_t floorShift(int64_t v, int64_t n);
};

}
