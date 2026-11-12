#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class NormalizationFormTable {
public:
  static llvm::ArrayRef<llvm::StringLiteral> getForms();
  static bool isKnown(llvm::StringRef form);
};

}
