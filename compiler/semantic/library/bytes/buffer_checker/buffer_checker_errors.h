#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <optional>
#include <string>

namespace tz {

enum class BufferCheckerCountOfBytesErrorCode {
  NOT_ONE_TO_SIX,
};

struct BufferCheckerCountOfBytesError {
  BufferCheckerCountOfBytesErrorCode code;
  std::string message;
};

enum class BufferCheckerDecodedInputErrorCode {
  NOT_BYTES,
};

struct BufferCheckerDecodedInputError {
  BufferCheckerDecodedInputErrorCode code;
  std::string message;
};

enum class BufferCheckerVarValueFitsErrorCode {
  VALUE_MAY_NOT_FIT,
};

struct BufferCheckerVarValueFitsError {
  BufferCheckerVarValueFitsErrorCode code;
  std::string message;
};

class ErrorTextBuffer {
public:
  static std::string valueArg(llvm::StringRef name, llvm::StringRef type);
  static std::string slice();
  static std::string position(llvm::StringRef name);
  static std::string encoding(llvm::StringRef name);
  static std::string withString();
  static std::string byteCount(llvm::StringRef name);
  static std::string varFits(int64_t count, llvm::StringRef range);
  static std::string decode(llvm::StringRef type);
  static std::string noMember(llvm::StringRef name, llvm::StringRef type);
  static std::string written(llvm::StringRef offset, llvm::StringRef length,
                             llvm::StringRef object);
};

class ErrorTextBufferLabel {
public:
  static std::string comparePosition(size_t index);
  static std::string copyPosition(size_t index);
  static std::string offset();
  static std::string end();
  static std::string length();
  static std::string byte();
  static std::string offsetOf(llvm::StringRef name);
  static std::string options();
};

}
