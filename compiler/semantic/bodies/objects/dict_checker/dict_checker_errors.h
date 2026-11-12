#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class DictCheckerResolveDictTypeErrorCode {
  NOT_A_DICT,
  WEAK_COLLECTION,
  WRONG_TYPE_ARGUMENTS,
  NUMBER_KEYS,
  OTHER_KEYS,
  BAD_KEY_TYPE,
};

struct DictCheckerResolveDictTypeError {
  DictCheckerResolveDictTypeErrorCode code;
  std::string message;
};

class ErrorTextDictType {
public:
  static std::string weak(llvm::StringRef name, llvm::StringRef plain);
  static std::string setArgs(llvm::StringRef name);
  static std::string mapArgs(llvm::StringRef name);
  static std::string recordArgs();
  static std::string numberKeys(llvm::StringRef key, llvm::StringRef value);
  static std::string otherKeys(llvm::StringRef key, llvm::StringRef value);
};

enum class DictCheckerCheckKeyTypeErrorCode {
  UNKNOWN_KEY,
  RESULT_KEY,
};

struct DictCheckerCheckKeyTypeError {
  DictCheckerCheckKeyTypeErrorCode code;
  std::string message;
};

class ErrorTextKeyType {
public:
  static std::string unknown();
  static std::string result();
  static std::string keyLabel();
};

enum class DictCheckerStringKeyErrorCode {
  NUMBER_KEY,
};

struct DictCheckerStringKeyError {
  DictCheckerStringKeyErrorCode code;
  std::string message;
};

class ErrorTextRecordKey {
public:
  static std::string number(llvm::StringRef type);
  static std::string numberIn(llvm::StringRef type);
};

enum class DictCheckerCheckDictMemberErrorCode {
  METHOD_VALUE,
  NO_SUCH_PROPERTY,
};

struct DictCheckerCheckDictMemberError {
  DictCheckerCheckDictMemberErrorCode code;
  std::string message;
};

class ErrorTextDictMember {
public:
  static std::string callOnly(llvm::StringRef text, llvm::StringRef name);
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type, llvm::StringRef text);
};

enum class DictCheckerCheckDictIndexErrorCode {
  MAP_INDEX,
  SET_INDEX,
};

struct DictCheckerCheckDictIndexError {
  DictCheckerCheckDictIndexErrorCode code;
  std::string message;
};

class ErrorTextDictIndex {
public:
  static std::string mapIndex(llvm::StringRef text);
  static std::string setIndex(llvm::StringRef text);
};

enum class DictCheckerCheckDictTargetErrorCode {
  MAP_TARGET,
  SET_SIZE,
  SET_INDEX,
  MAY_BE_MISSING,
};

struct DictCheckerCheckDictTargetError {
  DictCheckerCheckDictTargetErrorCode code;
  std::string message;
};

class ErrorTextDictTarget {
public:
  static std::string mapSet(llvm::StringRef text, llvm::StringRef access);
  static std::string setSize(llvm::StringRef text);
  static std::string setIndex(llvm::StringRef text);
  static std::string missing(llvm::StringRef key, llvm::StringRef hint);
  static std::string numberHint(llvm::StringRef key);
  static std::string textHint(llvm::StringRef key);
};

enum class DictCheckerCheckDictDeleteErrorCode {
  NOT_A_RECORD,
};

struct DictCheckerCheckDictDeleteError {
  DictCheckerCheckDictDeleteErrorCode code;
  std::string message;
};

class ErrorTextDictDelete {
public:
  static std::string entryPart(bool isMap);
  static std::string property(llvm::StringRef kind, llvm::StringRef text, llvm::StringRef what);
};

enum class DictCheckerCheckDictInErrorCode {
  MAY_BE_UNDEFINED,
  NOT_A_RECORD,
  NUMBER_KEY,
};

struct DictCheckerCheckDictInError {
  DictCheckerCheckDictInErrorCode code;
  std::string message;
};

class ErrorTextDictIn {
public:
  static std::string undefined();
  static std::string property(llvm::StringRef kind, llvm::StringRef text, llvm::StringRef what);
};

}
