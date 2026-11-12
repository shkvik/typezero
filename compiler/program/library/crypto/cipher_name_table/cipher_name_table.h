#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

enum class TurnedOffMatch {
  EXACT,
  PREFIX,
};

struct TurnedOffCipherInfo {
  llvm::StringLiteral text;
  TurnedOffMatch match;
};

struct CipherAliasInfo {
  llvm::StringLiteral alias;
  llvm::StringLiteral name;
};

class CipherNameTable {
public:
  static bool isKnown(llvm::StringRef name);
  static llvm::StringRef getCanonical(llvm::StringRef name);
  static bool isTurnedOff(llvm::StringRef name);
  static bool needsAuthTagLength(llvm::StringRef name);
};

}
