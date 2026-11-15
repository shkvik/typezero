#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <string>

namespace tz {

enum class LibraryJsonRegistryTypeOfAgreesErrorCode {
  NEVER_NULL,
  NEVER_ARRAY,
};

struct LibraryJsonRegistryTypeOfAgreesError {
  LibraryJsonRegistryTypeOfAgreesErrorCode code;
  std::string message;
};

enum class LibraryJsonRegistryNullPossibleErrorCode {
  NEVER_NULL,
};

struct LibraryJsonRegistryNullPossibleError {
  LibraryJsonRegistryNullPossibleErrorCode code;
  std::string message;
};

enum class LibraryJsonRegistryTestsUnknownErrorCode {
  KNOWN_TYPE,
};

struct LibraryJsonRegistryTestsUnknownError {
  LibraryJsonRegistryTestsUnknownErrorCode code;
  std::string message;
};

class ErrorTextJsonTest {
public:
  static std::string neverNull(llvm::StringRef x, bool isAnd, llvm::StringRef name);
  static std::string neverArray(llvm::StringRef x, bool isAnd, llvm::StringRef name);
};

class ErrorTextJsonKey {
public:
  static std::string index(llvm::StringRef type);
  static std::string literal(llvm::StringRef type);
  static std::string label(llvm::StringRef name);
};

class ErrorTextJsonMethod {
public:
  static std::string none(llvm::StringRef name, llvm::StringRef variable);
};

class ErrorTextJsonLiteral {
public:
  static std::string spread();
  static std::string function();
  static std::string defaulted(llvm::StringRef name);
};

class ErrorTextJsonNull {
public:
  static std::string never(llvm::StringRef type, llvm::StringRef op);
};

class ErrorTextJsonIsJson {
public:
  static std::string known(llvm::StringRef type, bool json);
};

enum class LibraryJsonRegistryEmbeddedFileErrorCode {
  NOT_CONSTANT,
  BACKSLASH,
  NOT_RELATIVE,
  UNREADABLE,
};

struct LibraryJsonRegistryEmbeddedFileError {
  LibraryJsonRegistryEmbeddedFileErrorCode code;
  std::string message;
};

enum class LibraryJsonRegistryConstantJsonErrorCode {
  BYTE_ORDER_MARK,
  INVALID,
};

struct LibraryJsonRegistryConstantJsonError {
  LibraryJsonRegistryConstantJsonErrorCode code;
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
  static std::string tooLarge(llvm::StringRef shown);
};

class ErrorTextConstantJson {
public:
  static std::string bom();
  static std::string invalid(llvm::StringRef problem, size_t line, size_t column);
};

}
