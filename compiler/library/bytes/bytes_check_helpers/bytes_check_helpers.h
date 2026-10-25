#pragma once

#include "compiler/check/sema/sema.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MathExtras.h"
#include <limits>

namespace tz {

// Functions that several classes made from sema_bytes.cpp use.

using Bytes = Type::Bytes;

using Range = ValueRange;

inline constexpr int64_t kIntMin = std::numeric_limits<int32_t>::min();

inline constexpr int64_t kIntMax = std::numeric_limits<int32_t>::max();

// Buffer's encodings, numbered as in runtime/library/bytes/buffer/buffer.c.
enum Encoding { Utf8, Utf16le, Latin1, Ascii, Base64, Base64url, Hex };

std::optional<int> encodingNamed(llvm::StringRef name);

// The text of a value for hints: a name, a number, the source of an
// operation; "x" for anything else.
std::string valueText(const Expr *e);

// How to cut a value to the bits of an element, as JavaScript does.
std::string wrapHint(Bytes kind, const std::string &value);

// The range of `a op b`, where integers of the type (an int with `isInt`)
// wrap around: a result that may leave its range is unknown.
Range operate(BinaryOp op, const Range &a, const Range &b, bool isInt);

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
bool numberMethod(llvm::StringRef name, bool dataView, NumberMethod &m);

// "a Uint16", as a value written goes into one.
std::string numberWhat(Bytes kind);

} // namespace tz
