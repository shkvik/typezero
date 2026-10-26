#pragma once

#include "compiler/check/checker/checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MathExtras.h"
#include <limits>

namespace tz {

using Bytes = Type::Bytes;

using Range = ValueRange;

inline constexpr int64_t kIntMin = std::numeric_limits<int32_t>::min();

inline constexpr int64_t kIntMax = std::numeric_limits<int32_t>::max();

enum Encoding { Utf8, Utf16le, Latin1, Ascii, Base64, Base64url, Hex };

std::optional<int> encodingNamed(llvm::StringRef name);

std::string valueText(const Expr *e);

std::string wrapHint(Bytes kind, const std::string &value);

Range operate(BinaryOp op, const Range &a, const Range &b, bool isInt);

struct NumberMethod {
  bool write = false;
  Bytes kind = Bytes::None;
  bool big = false;
  bool variable = false, isSigned = false;
  bool bigint = false;
};

bool numberMethod(llvm::StringRef name, bool dataView, NumberMethod &m);

std::string numberWhat(Bytes kind);

}
