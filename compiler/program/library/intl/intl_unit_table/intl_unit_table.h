#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class IntlUnitTable {
public:
  static bool isWellFormed(llvm::StringRef unit);
  static llvm::ArrayRef<llvm::StringLiteral> getRelativeUnits();

private:
  static bool isSanctioned(llvm::StringRef unit);
};

}
