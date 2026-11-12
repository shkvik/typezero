#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

class LibraryModuleNames {
public:
  static constexpr llvm::StringLiteral kSubtleName = "%subtle";
  static constexpr llvm::StringLiteral kIntlLocaleName = "%IntlLocale";
  static constexpr llvm::StringLiteral kNeverthrowPath = "neverthrow";
};

}
