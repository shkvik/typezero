#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

class LibraryModuleNames {
public:
  static constexpr llvm::StringLiteral kSubtleName = "%subtle";
  static constexpr llvm::StringLiteral kIntlLocaleName = "%IntlLocale";
  static constexpr llvm::StringLiteral kNeverthrowPath = "neverthrow";
  static constexpr llvm::StringLiteral kIntrinsicsPath = "tz:intrinsics";
  static constexpr llvm::StringLiteral kFsStreamsPath = "node:internal/fs/streams";
  static constexpr llvm::StringLiteral kStreamPath = "node:stream";
  static constexpr llvm::StringLiteral kStreamPromisesPath = "node:stream/promises";
};

}
