#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeConcatenationRegistryConvertibleErrorCode {
  VOID_VALUE,
  FUNCTION_VALUE,
  TUPLE_VALUE,
  RESULT_VALUE,
  OBJECT_TO_STRING,
  DICT_VALUE,
  ITERATOR_VALUE,
  UNKNOWN_VALUE,
  JSON_VALUE,
  OBJECT_VALUE,
  UNION_MEMBER,
};

struct CodeConcatenationRegistryConvertibleError {
  CodeConcatenationRegistryConvertibleErrorCode code;
  std::string message;
};

class ErrorTextToString {
public:
  static std::string voidValue();
  static std::string function();
  static std::string tuple();
  static std::string result();
  static std::string ownString(llvm::StringRef text);
  static std::string dict(llvm::StringRef kind, llvm::StringRef shown);
  static std::string iterator();
  static std::string unknown();
  static std::string json();
  static std::string object();
  static std::string member(llvm::StringRef type, llvm::StringRef member);
};

class ErrorTextMixedConcat {
public:
  static std::string operands(llvm::StringRef lhs, llvm::StringRef rhs);
  static std::string literal();
  static std::string convert(llvm::StringRef only);
  static std::string addOrJoin(llvm::StringRef sum, llvm::StringRef joined);
  static std::string asTemplate(llvm::StringRef joined);
  static std::string addRight(llvm::StringRef first, llvm::StringRef second);
  static std::string addLeft(llvm::StringRef first, llvm::StringRef second);
};

}
