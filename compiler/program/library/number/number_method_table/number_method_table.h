#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

enum class NumberMethod {
  TO_INT,
  TO_INT8,
  TO_INT16,
  TO_INT32,
  TO_UINT8,
  TO_UINT16,
  TO_UINT32,
  TO_UINT64,
  TO_DOUBLE,
  TO_DECIMAL,
  TO_STRING,
  TO_FIXED,
};

struct NumberMethodInfo {
  llvm::StringLiteral name;
  NumberMethod method;
  llvm::StringLiteral convertsTo;
};

class NumberMethodTable {
public:
  static const NumberMethodInfo *find(llvm::StringRef name);
};

}
