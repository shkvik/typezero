#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class BufferStaticCheckerSourceElementErrorCode {
  NOT_ITERABLE,
};

struct BufferStaticCheckerSourceElementError {
  BufferStaticCheckerSourceElementErrorCode code;
  std::string message;
};

enum class BufferStaticCheckerConcatSourcesErrorCode {
  NOT_BYTES_LIST,
};

struct BufferStaticCheckerConcatSourcesError {
  BufferStaticCheckerConcatSourcesErrorCode code;
  std::string message;
};

class ErrorTextBytesStatic {
public:
  static std::string missing(llvm::StringRef object, llvm::StringRef name);
  static std::string notYet(llvm::StringRef object, llvm::StringRef name);
  static std::string from(llvm::StringRef object);
  static std::string source(llvm::StringRef what, llvm::StringRef type);
};

class ErrorTextBufferStatic {
public:
  static std::string offsetOnly(llvm::StringRef type);
  static std::string allocFill(llvm::StringRef type);
  static std::string fillCoding();
  static std::string concat(llvm::StringRef type);
  static std::string byteLength(llvm::StringRef type);
  static std::string unknown(llvm::StringRef name);
  static std::string embedSize(llvm::StringRef shown);
  static std::string buffer();
  static std::string size();
  static std::string total();
};

}
