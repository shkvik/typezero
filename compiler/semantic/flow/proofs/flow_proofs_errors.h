#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class FlowProofsRequireBetweenErrorCode {
  EXEMPT,
  CONSTANT_NEGATIVE,
  CONSTANT_OUT_OF_RANGE,
  NOT_WHOLE_NUMBER,
  NOT_PROVEN,
};

struct FlowProofsRequireBetweenError {
  FlowProofsRequireBetweenErrorCode code;
  std::string message;
};

enum class FlowProofsRequireLessErrorCode {
  NOT_PROVEN,
};

struct FlowProofsRequireLessError {
  FlowProofsRequireLessErrorCode code;
  std::string message;
};

enum class FlowProofsRequireUpToErrorCode {
  EXEMPT,
  NOT_PROVEN,
};

struct FlowProofsRequireUpToError {
  FlowProofsRequireUpToErrorCode code;
  std::string message;
};

enum class FlowProofsValueFitsErrorCode {
  CONSTANT_DOES_NOT_FIT,
  FIT_NOT_PROVEN,
};

struct FlowProofsValueFitsError {
  FlowProofsValueFitsErrorCode code;
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

class ErrorTextLess {
public:
  static std::string unproven(llvm::StringRef what, llvm::StringRef left, llvm::StringRef right);
};

class ErrorTextUpTo {
public:
  static std::string limit(llvm::StringRef object, bool bytes, int64_t less);
  static std::string unproven(llvm::StringRef what, std::optional<std::string> value,
                              llvm::StringRef limit);
};

class ErrorTextBytes {
public:
  static std::string range(int64_t lo, int64_t hi);
};

class ErrorTextByteFit {
public:
  static std::string constant(int64_t value, llvm::StringRef what, llvm::StringRef range,
                              int64_t stored, llvm::StringRef hint);
  static std::string unproven(llvm::StringRef what, llvm::StringRef range, llvm::StringRef text,
                              int64_t lo, int64_t hi, llvm::StringRef hint);
};

}
