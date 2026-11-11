#include "compiler/program/library/math/bit_function_table/bit_function_table.h"

namespace tz {

namespace {

const BitFunctionInfo kBitFunctions[] = {
    {
        .name = "rotateLeft32",
        .builtin = Builtin::BitRotateLeft,
        .bits = 32,
        .args = 2,
        .counts = false,
        .wide = false,
        .first = "",
        .second = "",
    },
    {
        .name = "rotateLeft64",
        .builtin = Builtin::BitRotateLeft,
        .bits = 64,
        .args = 2,
        .counts = false,
        .wide = false,
        .first = "",
        .second = "",
    },
    {
        .name = "rotateRight32",
        .builtin = Builtin::BitRotateRight,
        .bits = 32,
        .args = 2,
        .counts = false,
        .wide = false,
        .first = "",
        .second = "",
    },
    {
        .name = "rotateRight64",
        .builtin = Builtin::BitRotateRight,
        .bits = 64,
        .args = 2,
        .counts = false,
        .wide = false,
        .first = "",
        .second = "",
    },
    {
        .name = "leadingZeros32",
        .builtin = Builtin::BitLeadingZeros,
        .bits = 32,
        .args = 1,
        .counts = true,
        .wide = false,
        .first = "",
        .second = "",
    },
    {
        .name = "leadingZeros64",
        .builtin = Builtin::BitLeadingZeros,
        .bits = 64,
        .args = 1,
        .counts = true,
        .wide = false,
        .first = "",
        .second = "",
    },
    {
        .name = "trailingZeros32",
        .builtin = Builtin::BitTrailingZeros,
        .bits = 32,
        .args = 1,
        .counts = true,
        .wide = false,
        .first = "",
        .second = "",
    },
    {
        .name = "trailingZeros64",
        .builtin = Builtin::BitTrailingZeros,
        .bits = 64,
        .args = 1,
        .counts = true,
        .wide = false,
        .first = "",
        .second = "",
    },
    {
        .name = "onesCount32",
        .builtin = Builtin::BitOnesCount,
        .bits = 32,
        .args = 1,
        .counts = true,
        .wide = false,
        .first = "",
        .second = "",
    },
    {
        .name = "onesCount64",
        .builtin = Builtin::BitOnesCount,
        .bits = 64,
        .args = 1,
        .counts = true,
        .wide = false,
        .first = "",
        .second = "",
    },
    {
        .name = "reverseBytes32",
        .builtin = Builtin::BitReverseBytes,
        .bits = 32,
        .args = 1,
        .counts = false,
        .wide = false,
        .first = "",
        .second = "",
    },
    {
        .name = "reverseBytes64",
        .builtin = Builtin::BitReverseBytes,
        .bits = 64,
        .args = 1,
        .counts = false,
        .wide = false,
        .first = "",
        .second = "",
    },
    {
        .name = "mul64",
        .builtin = Builtin::BitMul64,
        .bits = 64,
        .args = 2,
        .counts = false,
        .wide = true,
        .first = "hi",
        .second = "lo",
    },
    {
        .name = "add64",
        .builtin = Builtin::BitAdd64,
        .bits = 64,
        .args = 3,
        .counts = false,
        .wide = true,
        .first = "sum",
        .second = "carry",
    },
    {
        .name = "sub64",
        .builtin = Builtin::BitSub64,
        .bits = 64,
        .args = 3,
        .counts = false,
        .wide = true,
        .first = "diff",
        .second = "borrow",
    },
};

}

const BitFunctionInfo *BitFunctionTable::find(llvm::StringRef name) {
  for (const BitFunctionInfo &info : kBitFunctions) {
    if (info.name == name) {
      return &info;
    }
  }

  return nullptr;
}

}
