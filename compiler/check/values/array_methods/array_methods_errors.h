#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ArrayMethodsCheckArrayMethodErrorCode {
  WRONG_ARITY,
  RESULTS_COMPARED,
  ELEMENTS_NOT_TEXT,
  NEEDS_COMPARISON,
  NO_SUCH_PROPERTY,
};

struct ArrayMethodsCheckArrayMethodError {
  ArrayMethodsCheckArrayMethodErrorCode code;
  std::string message;
};

class ErrorTextArrayMethod {
public:
  static std::string results(llvm::StringRef name);
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type);
};

enum class ArrayMethodsTextElementsErrorCode {
  FUNCTIONS,
  RESULTS,
  UNKNOWN,
  JSON,
  OBJECTS,
};

struct ArrayMethodsTextElementsError {
  ArrayMethodsTextElementsErrorCode code;
  std::string message;
};

class ErrorTextArrayText {
public:
  static std::string functions();
  static std::string results(llvm::StringRef name);
  static std::string unknown(llvm::StringRef name);
  static std::string json(llvm::StringRef name);
  static std::string objects(llvm::StringRef name);
};

enum class ArrayMethodsSortableErrorCode {
  FUNCTIONS,
  UNKNOWN,
  JSON,
  DATES,
  OBJECTS,
  MIXED_UNION,
  NUMBERS,
};

struct ArrayMethodsSortableError {
  ArrayMethodsSortableErrorCode code;
  std::string message;
};

class ErrorTextArraySort {
public:
  static std::string functions();
  static std::string unknown();
  static std::string json();
  static std::string dates();
  static std::string objects();
  static std::string mixed(llvm::StringRef type);
  static std::string numbers(bool optional);
};

}
