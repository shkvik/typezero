#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class DictMethodsCheckDictMethodErrorCode {
  HAS_OWN_PROPERTY,
  MAY_BE_UNDEFINED,
  NO_RECORD_METHODS,
  WRONG_ARITY,
  FOR_EACH_THIS,
  ALREADY_REPORTED,
  NOT_A_SET,
  NO_SUCH_PROPERTY,
};

struct DictMethodsCheckDictMethodError {
  DictMethodsCheckDictMethodErrorCode code;
  std::string message;
};

class ErrorTextRecordMethod {
public:
  static std::string hasOwn(llvm::StringRef key, llvm::StringRef text);
  static std::string undefined(llvm::StringRef key);
  static std::string noMethods(llvm::StringRef key, llvm::StringRef value, llvm::StringRef text);
};

class ErrorTextDictForEach {
public:
  static std::string thisArg();
};

class ErrorTextSetCombine {
public:
  static std::string notSet(llvm::StringRef name, llvm::StringRef key, llvm::StringRef other);
};

class ErrorTextDictMethod {
public:
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type, llvm::StringRef text,
                                bool isMap);
};

enum class DictMethodsCheckFromEntriesErrorCode {
  ALREADY_REPORTED,
  WRONG_ARITY,
  KEYS_NOT_STRINGS,
  VALUES_UNDEFINED,
};

struct DictMethodsCheckFromEntriesError {
  DictMethodsCheckFromEntriesErrorCode code;
  std::string message;
};

class ErrorTextFromEntries {
public:
  static std::string keys(llvm::StringRef type);
  static std::string undefined();
};

enum class DictMethodsCheckGroupByErrorCode {
  ALREADY_REPORTED,
  WRONG_ARITY,
  NO_GROUP_KEY,
  KEYS_NOT_STRINGS,
  BAD_KEY_TYPE,
};

struct DictMethodsCheckGroupByError {
  DictMethodsCheckGroupByErrorCode code;
  std::string message;
};

class ErrorTextGroupBy {
public:
  static std::string noKey(llvm::StringRef fn);
  static std::string keys(llvm::StringRef type);
};

enum class DictMethodsCheckMapStaticErrorCode {
  ALREADY_REPORTED,
  WRONG_ARITY,
  WRONG_GROUP_KEY,
  NOT_SUPPORTED,
};

struct DictMethodsCheckMapStaticError {
  DictMethodsCheckMapStaticErrorCode code;
  std::string message;
};

class ErrorTextMapStatic {
public:
  static std::string unknown(llvm::StringRef name);
};

enum class DictMethodsCheckDictObjectCallErrorCode {
  ALWAYS_EMPTY,
};

struct DictMethodsCheckDictObjectCallError {
  DictMethodsCheckDictObjectCallErrorCode code;
  std::string message;
};

class ErrorTextDictObject {
public:
  static std::string empty(llvm::StringRef fn, llvm::StringRef kind, llvm::StringRef text,
                           llvm::StringRef method);
};

}
