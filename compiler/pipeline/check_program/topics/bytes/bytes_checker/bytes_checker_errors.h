#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <optional>
#include <string>

namespace tz {

enum class BytesCheckerKnownEncodingErrorCode {
  UNKNOWN_ENCODING,
};

struct BytesCheckerKnownEncodingError {
  BytesCheckerKnownEncodingErrorCode code;
  std::string message;
};

enum class BytesCheckerBytesIndexingErrorCode {
  NOT_INDEXED,
};

struct BytesCheckerBytesIndexingError {
  BytesCheckerBytesIndexingErrorCode code;
  std::string message;
};

class ErrorTextBytesIndex {
public:
  static std::string notIndexed(const Type *type);
  static std::string store(llvm::StringRef target, llvm::StringRef object, llvm::StringRef position,
                           std::optional<int64_t> at);
};

class ErrorTextBytesMember {
public:
  static std::string deprecated(llvm::StringRef name);
  static std::string noMember(llvm::StringRef name, llvm::StringRef type);
  static std::string notCalled(llvm::StringRef name, llvm::StringRef type);
  static std::string narrow(llvm::StringRef name, llvm::StringRef type);
  static std::string lacking(llvm::StringRef name, llvm::StringRef lacking, llvm::StringRef type,
                             llvm::StringRef with);
  static std::string callOnly(llvm::StringRef object, llvm::StringRef name);
};

class ErrorTextBytesString {
public:
  static std::string noText(llvm::StringRef type, bool buffer);
};

class ErrorTextBufferEncoding {
public:
  static std::string unknown(llvm::StringRef name);
  static std::string notKnown();
};

class ErrorTextArrayBuffer {
public:
  static std::string notYet(llvm::StringRef name);
  static std::string noMember(llvm::StringRef name);
};

}
