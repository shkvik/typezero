#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LateClassesBaseClassErrorCode {
  ALREADY_REPORTED,
  KNOWN_ONLY_AT_RUN,
  NOT_A_CLASS,
  JAVASCRIPT_CLASS,
  GENERIC_CLASS,
  MADE_BY_FS,
};

struct LateClassesBaseClassError {
  LateClassesBaseClassErrorCode code;
  std::string message;
};

class ErrorTextBaseClass {
public:
  static std::string atRun(llvm::StringRef type);
  static std::string notClass(llvm::StringRef type);
  static std::string javascript(llvm::StringRef className);
  static std::string generic(llvm::StringRef className);
  static std::string madeByFs(llvm::StringRef className);
};

enum class LateClassesStaticTypeErrorCode {
  UNKNOWN_FROM_VALUE,
};

struct LateClassesStaticTypeError {
  LateClassesStaticTypeErrorCode code;
  std::string message;
};

class ErrorTextStaticType {
public:
  static std::string unknown(llvm::StringRef field);
};

enum class LateClassesOldObjectErrorCode {
  HOLDS_OLD_OBJECT,
};

struct LateClassesOldObjectError {
  LateClassesOldObjectErrorCode code;
  std::string message;
};

class ErrorTextOldObject {
public:
  static std::string held(llvm::StringRef className, llvm::StringRef field);
  static std::string decorator();
};

enum class LateClassesReplacedFieldErrorCode {
  FIELD_IN_REPLACEMENT,
};

struct LateClassesReplacedFieldError {
  LateClassesReplacedFieldErrorCode code;
  std::string message;
};

class ErrorTextReplacedField {
public:
  static std::string clash(llvm::StringRef child, llvm::StringRef className, llvm::StringRef field);
};

}
