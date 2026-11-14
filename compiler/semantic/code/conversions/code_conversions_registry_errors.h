#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeConversionsRegistryRangeProvenErrorCode {
  LITERAL_OUT_OF_RANGE,
  MAY_BE_OUT_OF_RANGE,
};

struct CodeConversionsRegistryRangeProvenError {
  CodeConversionsRegistryRangeProvenErrorCode code;
  std::string message;
};

enum class CodeConversionsRegistryAssignableErrorCode {
  GENERIC_CLASS_VALUE,
  UNKNOWN_VALUE,
  LITERAL_NOT_INSTANCE,
  LITERAL_WITH_METHODS,
  GENERIC_WIDENING,
  CLASS_NOT_EXTENDED,
  CLASS_NOT_IMPLEMENTED,
  OBJECT_INTO_UNION,
  TUPLE_NOT_ARRAY,
  OBJECT_MISMATCH,
  MAY_BE_UNDEFINED,
  JSON_UNCHECKED,
  NOT_AN_OBJECT,
  OBJECT_INTO_JSON,
  WIDE_INTEGERS_INTO_JSON,
  DECIMAL_INTO_JSON,
  DOUBLE_INTO_DECIMAL,
  DECIMAL_INTO_NUMBER,
  NOT_ASSIGNABLE,
};

struct CodeConversionsRegistryAssignableError {
  CodeConversionsRegistryAssignableErrorCode code;
  std::string message;
};

class ErrorTextMismatch {
public:
  static std::string extraField(llvm::StringRef name);
  static std::string noField(llvm::StringRef name);
  static std::string fieldType(llvm::StringRef name, llvm::StringRef here, llvm::StringRef there,
                               llvm::StringRef to);
  static std::string typeHint(llvm::StringRef to);
  static std::string readonly(llvm::StringRef name, llvm::StringRef to);
};

class ErrorTextUnknownHint {
public:
  static std::string instanceOf(llvm::StringRef value, llvm::StringRef cls);
  static std::string typeOf(llvm::StringRef value, llvm::StringRef type);
  static std::string isArray(llvm::StringRef value);
  static std::string anyClass(llvm::StringRef check);
  static std::string anything(llvm::StringRef check);
};

class ErrorTextRange {
public:
  static std::string holds(llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals);
  static std::string outside(llvm::StringRef check);
  static std::string status(llvm::StringRef value);
  static std::string between(llvm::StringRef value, llvm::StringRef lo, llvm::StringRef hi);
};

class ErrorTextAssignable {
public:
  static std::string notTo(llvm::StringRef from, llvm::StringRef to, llvm::StringRef what);
  static std::string functions();
  static std::string stream();
  static std::string request();
  static std::string never();
  static std::string extendsBus(llvm::StringRef name);
  static std::string extendsInt(llvm::StringRef name);
  static std::string generic(llvm::StringRef name, llvm::StringRef example);
  static std::string literal(llvm::StringRef to);
  static std::string methods();
  static std::string classes(llvm::StringRef from, llvm::StringRef to);
  static std::string implements(llvm::StringRef to);
  static std::string inUnion();
  static std::string withValue(llvm::StringRef field);
  static std::string tuple();
  static std::string both(llvm::StringRef other);
  static std::string undefined();
  static std::string json();
  static std::string object();
  static std::string jsonObject();
  static std::string wideJson();
  static std::string noDecimal();
  static std::string toDecimal();
  static std::string toNumber();
  static std::string alsoHere(llvm::StringRef other);
};

class ErrorTextConvert {
public:
  static std::string direct(llvm::StringRef text, llvm::StringRef method);
  static std::string rounded(llvm::StringRef text, llvm::StringRef method);
};

}
