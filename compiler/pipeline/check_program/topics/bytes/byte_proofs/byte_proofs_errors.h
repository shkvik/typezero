#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>

namespace tz {

enum class ByteProofsValueFitsErrorCode {
  CONSTANT_DOES_NOT_FIT,
  FIT_NOT_PROVEN,
};

struct ByteProofsValueFitsError {
  ByteProofsValueFitsErrorCode code;
  std::string message;
};

enum class ByteProofsOffsetAlignedErrorCode {
  MISALIGNED_OFFSET,
};

struct ByteProofsOffsetAlignedError {
  ByteProofsOffsetAlignedErrorCode code;
  std::string message;
};

enum class ByteProofsBufferAlignedErrorCode {
  SIZE_NOT_PROVEN,
};

struct ByteProofsBufferAlignedError {
  ByteProofsBufferAlignedErrorCode code;
  std::string message;
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

class ErrorTextByteView {
public:
  static std::string offset(llvm::StringRef what, int64_t size);
  static std::string size(llvm::StringRef what, int64_t size);
  static std::string end(llvm::StringRef offset, llvm::StringRef length, int64_t size);
  static std::string notFits(llvm::StringRef what, llvm::StringRef end, llvm::StringRef buffer);
  static std::string element(llvm::StringRef type);
  static std::string offsetOf(llvm::StringRef what);
  static std::string lengthOf(llvm::StringRef what);
};

}
