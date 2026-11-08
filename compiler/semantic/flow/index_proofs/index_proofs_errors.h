#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <optional>
#include <string>

namespace tz {

enum class IndexProofsRequireBetweenErrorCode {
  EXEMPT,
  CONSTANT_NEGATIVE,
  CONSTANT_OUT_OF_RANGE,
  NOT_WHOLE_NUMBER,
  NOT_PROVEN,
};

struct IndexProofsRequireBetweenError {
  IndexProofsRequireBetweenErrorCode code;
  std::string message;
};

class ErrorTextBetween {
public:
  static std::string range(int64_t lo, std::optional<int64_t> hi);
  static std::string negative(llvm::StringRef what, int64_t found);
  static std::string outside(llvm::StringRef what, llvm::StringRef range, int64_t found);
  static std::string notWhole(llvm::StringRef what, llvm::StringRef range, llvm::StringRef type);
  static std::string test(llvm::StringRef value, int64_t lo, std::optional<int64_t> hi);
  static std::string unproven(llvm::StringRef what, llvm::StringRef range, llvm::StringRef test);
};

enum class IndexProofsRequireLessErrorCode {
  NOT_PROVEN,
};

struct IndexProofsRequireLessError {
  IndexProofsRequireLessErrorCode code;
  std::string message;
};

class ErrorTextLess {
public:
  static std::string unproven(llvm::StringRef what, llvm::StringRef left, llvm::StringRef right);
};

enum class IndexProofsRequireUpToErrorCode {
  EXEMPT,
  NOT_PROVEN,
};

struct IndexProofsRequireUpToError {
  IndexProofsRequireUpToErrorCode code;
  std::string message;
};

class ErrorTextUpTo {
public:
  static std::string limit(llvm::StringRef object, bool bytes, int64_t less);
  static std::string unproven(llvm::StringRef what, std::optional<std::string> value,
                              llvm::StringRef limit);
};

}
