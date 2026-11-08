#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class FsCheckerOctalModeErrorCode {
  NOT_OCTAL,
};

struct FsCheckerOctalModeError {
  FsCheckerOctalModeErrorCode code;
  std::string message;
};

class ErrorTextFsOptions {
public:
  static std::string literal(llvm::StringRef fn, llvm::StringRef first);
  static std::string byName(llvm::StringRef fn, llvm::StringRef first);
  static std::string unknown(llvm::StringRef fn, llvm::StringRef name,
                             llvm::ArrayRef<llvm::StringRef> allowed);
  static std::string flag(llvm::StringRef fn, llvm::StringRef option);
};

class ErrorTextFsEncoding {
public:
  static std::string literal(llvm::StringRef fn);
  static std::string utf8Only(llvm::StringRef fn, llvm::StringRef encoding);
  static std::string dependsOn(llvm::StringRef fn);
  static std::string forText(llvm::StringRef fn);
};

class ErrorTextFsCall {
public:
  static std::string notFunc(llvm::StringRef fn);
  static std::string zeroPath();
  static std::string flag(llvm::StringRef fn, llvm::StringRef written, bool literal);
  static std::string data(llvm::StringRef fn, const Type *type);
  static std::string chmod();
  static std::string bigint();
  static std::string rmdir();
  static std::string copyMode(llvm::StringRef fn);
};

class ErrorTextFsConstant {
public:
  static std::string value();
  static std::string notYet(llvm::StringRef name);
};

}
