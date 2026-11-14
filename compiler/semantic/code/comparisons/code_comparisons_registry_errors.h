#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeComparisonsRegistryCheckTypeOfNameErrorCode {
  ALREADY_REPORTED,
  NO_NUMBER,
  NO_SYMBOL,
  NOT_A_NAME,
  NEVER_MATCHES,
};

struct CodeComparisonsRegistryCheckTypeOfNameError {
  CodeComparisonsRegistryCheckTypeOfNameErrorCode code;
  std::string message;
};

enum class CodeComparisonsRegistryNoResultsErrorCode {
  RESULTS_COMPARED,
};

struct CodeComparisonsRegistryNoResultsError {
  CodeComparisonsRegistryNoResultsErrorCode code;
  std::string message;
};

enum class CodeComparisonsRegistryCheckInErrorCode {
  DICT_UNDEFINED,
  DICT_NUMBER_KEY,
  DICT_NOT_RECORD,
  UNION_UNDEFINED,
  UNION_NO_KEY,
  OPTIONAL_FIELD,
  ALWAYS_SAME,
  TUPLE_IN,
  OBJECT_OPTIONAL,
  OBJECT_ALWAYS_TRUE,
  OBJECT_ALWAYS_FALSE,
  OBJECT_IN,
  OPERAND_TYPES,
  ARRAY_IN,
};

struct CodeComparisonsRegistryCheckInError {
  CodeComparisonsRegistryCheckInErrorCode code;
  std::string message;
};

class ErrorTextTypeOfName {
public:
  static std::string noNumber(llvm::StringRef here);
  static std::string forType(llvm::StringRef type, llvm::StringRef names);
  static std::string noSymbol(llvm::StringRef names);
  static std::string notName(llvm::StringRef name, llvm::StringRef names);
  static std::string never(llvm::StringRef type, llvm::StringRef names, llvm::StringRef name);
  static std::string bigint();
};

class ErrorTextResultEquality {
public:
  static std::string compared(llvm::StringRef op);
};

class ErrorTextIn {
public:
  static std::string undefined();
  static std::string byLiteral();
  static std::string maybeUndef(llvm::StringRef name, llvm::StringRef member, llvm::StringRef text);
  static std::string always(bool with, llvm::StringRef unionName, llvm::StringRef name);
  static std::string tuple();
  static std::string objMaybe(llvm::StringRef text);
  static std::string objTrue();
  static std::string objFalse(llvm::StringRef type, llvm::StringRef name);
  static std::string objPlain();
  static std::string array(llvm::StringRef check);
};

}
