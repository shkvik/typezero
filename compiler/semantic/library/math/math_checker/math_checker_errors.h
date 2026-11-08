#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class MathCheckerWrapBitsWidthErrorCode {
  NO_MEMBER,
};

struct MathCheckerWrapBitsWidthError {
  MathCheckerWrapBitsWidthErrorCode code;
  std::string message;
};

enum class MathCheckerRoundedDoubleErrorCode {
  NO_FRACTION,
};

struct MathCheckerRoundedDoubleError {
  MathCheckerRoundedDoubleErrorCode code;
  std::string message;
};

class ErrorTextMath {
public:
  static std::string constant(llvm::StringRef name);
  static std::string noMember(llvm::StringRef name);
  static std::string noValues(llvm::StringRef name);
  static std::string rounding(llvm::StringRef name, llvm::StringRef type);
  static std::string oneType(llvm::StringRef common, llvm::StringRef type, llvm::StringRef hint);
  static std::string spread(llvm::StringRef type);
  static std::string spreadType(bool min, llvm::StringRef common, llvm::StringRef type);
};

class ErrorTextBit {
public:
  static std::string noMember(llvm::StringRef name);
  static std::string onlyFrom(llvm::StringRef name);
  static std::string fromInt(llvm::StringRef name, llvm::StringRef type);
  static std::string label(llvm::StringRef name);
  static std::string uint64(llvm::StringRef what, llvm::StringRef type, llvm::StringRef hint);
  static std::string count(llvm::StringRef what, llvm::StringRef type);
};

class ErrorTextMathArg {
public:
  static std::string notDouble(const Type *type);
  static std::string bits(llvm::StringRef what, unsigned bits, llvm::StringRef type,
                          llvm::StringRef hint);
  static std::string trunc(llvm::StringRef method);
};

}
