#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class StreamPipelineWritableEndErrorCode {
  NOT_WRITABLE,
};

struct StreamPipelineWritableEndError {
  StreamPipelineWritableEndErrorCode code;
  std::string message;
};

enum class StreamPipelineReadableEndErrorCode {
  NOT_READABLE,
};

struct StreamPipelineReadableEndError {
  StreamPipelineReadableEndErrorCode code;
  std::string message;
};

class ErrorTextPipeline {
public:
  static std::string notWritten(llvm::StringRef type);
  static std::string chunkKind(llvm::StringRef fn, llvm::StringRef chunk, llvm::StringRef type,
                               llvm::StringRef takes);
  static std::string notRead(llvm::StringRef type);
  static std::string source();
  static std::string function(bool last);
  static std::string stage(llvm::StringRef type);
  static std::string callback();
  static std::string streams();
};

class ErrorTextCompose {
public:
  static std::string streams();
  static std::string notStream(llvm::StringRef type);
};

}
