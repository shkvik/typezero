#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class BytesNewCheckerDecoderLabelErrorCode {
  UNKNOWN_LABEL,
};

struct BytesNewCheckerDecoderLabelError {
  BytesNewCheckerDecoderLabelErrorCode code;
  std::string message;
};

enum class BytesNewCheckerViewedBufferErrorCode {
  NOT_A_BUFFER,
};

struct BytesNewCheckerViewedBufferError {
  BytesNewCheckerViewedBufferErrorCode code;
  std::string message;
};

class ErrorTextBytesNew {
public:
  static std::string typeArgs(llvm::StringRef type);
  static std::string buffer();
  static std::string resizable();
  static std::string bufferLen();
  static std::string viewOver(llvm::StringRef type, bool typed);
  static std::string dataView();
  static std::string fatal();
  static std::string the(llvm::StringRef type);
  static std::string offsetOnly(llvm::StringRef type, llvm::StringRef from);
  static std::string lengthOf(llvm::StringRef type);
  static std::string floatLen();
};

class ErrorTextTextDecoder {
public:
  static std::string unknown(llvm::StringRef label);
  static std::string notKnown();
  static std::string options();
};

class ErrorTextBytesSource {
public:
  static std::string notSource(llvm::StringRef target, llvm::StringRef from, bool string);
  static std::string quoted(llvm::StringRef target);
  static std::string notNumbers(llvm::StringRef from, llvm::StringRef target);
  static std::string doubles(llvm::StringRef from, llvm::StringRef target, llvm::StringRef text);
  static std::string hint(llvm::StringRef target, llvm::StringRef text, llvm::StringRef wrap);
  static std::string mayNotFit(llvm::StringRef from, llvm::StringRef fromRange,
                               llvm::StringRef target, llvm::StringRef range, llvm::StringRef hint);
  static std::string unproven(llvm::StringRef from, llvm::StringRef target, llvm::StringRef range,
                              llvm::StringRef hint);
};

}
