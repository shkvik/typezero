#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

struct CurveNameInfo {
  llvm::StringLiteral name;
  bool isWebCrypto;
};

class CurveNameTable {
public:
  static bool isKnown(llvm::StringRef name);
  static bool isWebCryptoCurve(llvm::StringRef name);
};

}
