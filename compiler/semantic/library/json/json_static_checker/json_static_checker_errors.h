#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <string>

namespace tz {

enum class JsonStaticCheckerEmbeddedPathErrorCode {
  BACKSLASH,
  NOT_RELATIVE,
};

struct JsonStaticCheckerEmbeddedPathError {
  JsonStaticCheckerEmbeddedPathErrorCode code;
  std::string message;
};

enum class JsonStaticCheckerConstantJsonErrorCode {
  BYTE_ORDER_MARK,
  INVALID,
};

struct JsonStaticCheckerConstantJsonError {
  JsonStaticCheckerConstantJsonErrorCode code;
  std::string message;
};

class ErrorTextJsonStatic {
public:
  static std::string reviver();
  static std::string parseTypes();
  static std::string typeArgs(llvm::StringRef fn);
  static std::string noMember(llvm::StringRef fn);
  static std::string voidValue();
  static std::string toJson(llvm::StringRef cls);
  static std::string replacer();
  static std::string space(llvm::StringRef type);
};

class ErrorTextEmbed {
public:
  static std::string example(llvm::StringRef fn);
  static std::string literal(llvm::StringRef fn, llvm::StringRef example);
  static std::string backslash();
  static std::string relative(llvm::StringRef fn, llvm::StringRef example);
  static std::string unread(llvm::StringRef shown, llvm::StringRef why);
  static std::string typeArgs(llvm::StringRef fn);
  static std::string notUtf8(llvm::StringRef shown);
};

class ErrorTextConstantJson {
public:
  static std::string bom();
  static std::string invalid(llvm::StringRef problem, size_t line, size_t column);
};

}
