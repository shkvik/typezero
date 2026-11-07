#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class SetEncodingCheckerDecodedChunksErrorCode {
  NOT_READABLE,
  OBJECT_MODE,
};

struct SetEncodingCheckerDecodedChunksError {
  SetEncodingCheckerDecodedChunksErrorCode code;
  std::string message;
};

enum class SetEncodingCheckerStdinPlaceErrorCode {
  NOT_TOP_LEVEL,
};

struct SetEncodingCheckerStdinPlaceError {
  SetEncodingCheckerStdinPlaceErrorCode code;
  std::string message;
};

class ErrorTextSetEncoding {
public:
  static std::string unreadable(llvm::StringRef type);
  static std::string objectMode(llvm::StringRef chunk);
  static std::string stdinPlace();
  static std::string changes(llvm::StringRef why);
  static std::string usedBefore(llvm::StringRef how);
  static std::string usedHere();
};

class ErrorTextEncodingWhy {
public:
  static std::string noConstant();
  static std::string notConst(llvm::StringRef name);
  static std::string fieldSet(llvm::StringRef field);
  static std::string ownBlock(llvm::StringRef name);
};

class ErrorTextEncodingUse {
public:
  static std::string listeners(llvm::StringRef method);
  static std::string function();
  static std::string elsewhere();
};

}
