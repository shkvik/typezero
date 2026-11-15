#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LibraryCommonArityErrorCode {
  WRONG_COUNT,
  WRONG_COUNT_TAGGED,
};

struct LibraryCommonArityError {
  LibraryCommonArityErrorCode code;
  std::string message;
};

enum class LibraryCommonSpreadFreeErrorCode {
  SPREAD_ARGUMENT,
};

struct LibraryCommonSpreadFreeError {
  LibraryCommonSpreadFreeErrorCode code;
  std::string message;
};

enum class LibraryCommonKnownEncodingErrorCode {
  UNKNOWN_ENCODING,
};

struct LibraryCommonKnownEncodingError {
  LibraryCommonKnownEncodingErrorCode code;
  std::string message;
};

class ErrorTextNodeOptions {
public:
  static std::string literal(llvm::StringRef fn, llvm::StringRef first);
  static std::string byName(llvm::StringRef fn, llvm::StringRef first);
  static std::string unknown(llvm::StringRef fn, llvm::StringRef name,
                             llvm::ArrayRef<llvm::StringRef> allowed);
  static std::string flag(llvm::StringRef fn, llvm::StringRef option);
};

class ErrorTextBufferEncoding {
public:
  static std::string unknown(llvm::StringRef name);
  static std::string notKnown();
};

class ErrorTextIntrinsicCall {
public:
  static std::string wrong(llvm::StringRef name);
};

}
