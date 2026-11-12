#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>

namespace tz {

enum class ConstantFolderCheckDivisorErrorCode {
  DIVISION_BY_ZERO,
};

struct ConstantFolderCheckDivisorError {
  ConstantFolderCheckDivisorErrorCode code;
  std::string message;
};

class ErrorTextDivisor {
public:
  static std::string zero();
};

enum class ConstantFolderShiftCountErrorCode {
  OUTSIDE_BITS,
};

struct ConstantFolderShiftCountError {
  ConstantFolderShiftCountErrorCode code;
  std::string message;
};

class ErrorTextShiftCount {
public:
  static std::string outside(int64_t count, int last, llvm::StringRef type);
};

class ErrorTextOverflow {
public:
  static std::string inInt(llvm::StringRef value, llvm::StringRef type, int64_t wrapped);
  static std::string inDouble(llvm::StringRef value, llvm::StringRef type, int64_t wrapped);
};

}
