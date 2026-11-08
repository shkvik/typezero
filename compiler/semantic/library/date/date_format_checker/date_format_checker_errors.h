#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <string>

namespace tz {

enum class DateFormatCheckerOptionsObjectErrorCode {
  NOT_AN_OBJECT,
};

struct DateFormatCheckerOptionsObjectError {
  DateFormatCheckerOptionsObjectErrorCode code;
  std::string message;
};

class ErrorTextDateOptions {
public:
  static std::string notObject(llvm::StringRef method, llvm::StringRef type);
  static std::string unknown(llvm::StringRef name, llvm::StringRef method);
  static std::string kind(llvm::StringRef name, bool boolean, bool number, llvm::StringRef type);
  static std::string styleOnly(llvm::StringRef name, llvm::StringRef method);
  static std::string styleParts();
  static std::string digitsName();
  static std::string digits();
  static std::string unknownVal(llvm::StringRef name, llvm::StringRef value, llvm::StringRef first,
                                llvm::StringRef second);
  static std::string badValue(llvm::StringRef text, llvm::StringRef name,
                              llvm::ArrayRef<llvm::StringLiteral> allowed);
};

class ErrorTextDateFormat {
public:
  static std::string arity(size_t count);
  static std::string formatArg(llvm::StringRef name, llvm::StringRef type);
  static std::string range(llvm::StringRef name);
  static std::string noMember(llvm::StringRef name);
};

}
