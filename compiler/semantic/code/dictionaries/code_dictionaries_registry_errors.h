#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeDictionariesRegistryCheckKeyTypeErrorCode {
  UNKNOWN_KEY,
  RESULT_KEY,
};

struct CodeDictionariesRegistryCheckKeyTypeError {
  CodeDictionariesRegistryCheckKeyTypeErrorCode code;
  std::string message;
};

enum class CodeDictionariesRegistryStringKeyErrorCode {
  NUMBER_KEY,
};

struct CodeDictionariesRegistryStringKeyError {
  CodeDictionariesRegistryStringKeyErrorCode code;
  std::string message;
};

enum class CodeDictionariesRegistryCheckDictMemberErrorCode {
  METHOD_VALUE,
  NO_SUCH_PROPERTY,
};

struct CodeDictionariesRegistryCheckDictMemberError {
  CodeDictionariesRegistryCheckDictMemberErrorCode code;
  std::string message;
};

enum class CodeDictionariesRegistryCheckDictIndexErrorCode {
  MAP_INDEX,
  SET_INDEX,
};

struct CodeDictionariesRegistryCheckDictIndexError {
  CodeDictionariesRegistryCheckDictIndexErrorCode code;
  std::string message;
};

enum class CodeDictionariesRegistryCheckDictTargetErrorCode {
  MAP_TARGET,
  SET_SIZE,
  SET_INDEX,
  MAY_BE_MISSING,
};

struct CodeDictionariesRegistryCheckDictTargetError {
  CodeDictionariesRegistryCheckDictTargetErrorCode code;
  std::string message;
};

enum class CodeDictionariesRegistryCheckDictDeleteErrorCode {
  NOT_A_RECORD,
};

struct CodeDictionariesRegistryCheckDictDeleteError {
  CodeDictionariesRegistryCheckDictDeleteErrorCode code;
  std::string message;
};

enum class CodeDictionariesRegistryCheckDictInErrorCode {
  MAY_BE_UNDEFINED,
  NOT_A_RECORD,
  NUMBER_KEY,
};

struct CodeDictionariesRegistryCheckDictInError {
  CodeDictionariesRegistryCheckDictInErrorCode code;
  std::string message;
};

enum class CodeDictionariesRegistryPartsJoinErrorCode {
  KEYS_DIFFER,
  VALUES_DIFFER,
};

struct CodeDictionariesRegistryPartsJoinError {
  CodeDictionariesRegistryPartsJoinErrorCode code;
  std::string message;
};

enum class CodeDictionariesRegistryPairsKnownErrorCode {
  TYPES_UNDEFINED,
};

struct CodeDictionariesRegistryPairsKnownError {
  CodeDictionariesRegistryPairsKnownErrorCode code;
  std::string message;
};

enum class CodeDictionariesRegistrySourceShapeErrorCode {
  NOT_ITERABLE,
  NOT_PAIRS,
  WRONG_VALUES,
};

struct CodeDictionariesRegistrySourceShapeError {
  CodeDictionariesRegistrySourceShapeErrorCode code;
  std::string message;
};

enum class CodeDictionariesRegistryArgumentCountErrorCode {
  TOO_MANY_ARGUMENTS,
};

struct CodeDictionariesRegistryArgumentCountError {
  CodeDictionariesRegistryArgumentCountErrorCode code;
  std::string message;
};

enum class CodeDictionariesRegistryTypesGivenErrorCode {
  MAP_NEEDS_TYPES,
  SET_NEEDS_TYPES,
  UNDEFINED_NEEDS_TYPES,
};

struct CodeDictionariesRegistryTypesGivenError {
  CodeDictionariesRegistryTypesGivenErrorCode code;
  std::string message;
};

enum class CodeDictionariesRegistryPairShapeErrorCode {
  NOT_PAIRS,
};

struct CodeDictionariesRegistryPairShapeError {
  CodeDictionariesRegistryPairShapeErrorCode code;
  std::string message;
};

enum class CodeDictionariesRegistryElementsKnownErrorCode {
  TYPES_UNDEFINED,
};

struct CodeDictionariesRegistryElementsKnownError {
  CodeDictionariesRegistryElementsKnownErrorCode code;
  std::string message;
};

enum class CodeDictionariesRegistryResolveDictTypeErrorCode {
  WEAK_COLLECTION,
  WRONG_TYPE_ARGUMENTS,
  NUMBER_KEYS,
  OTHER_KEYS,
  BAD_KEY_TYPE,
};

struct CodeDictionariesRegistryResolveDictTypeError {
  CodeDictionariesRegistryResolveDictTypeErrorCode code;
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

class ErrorTextKeyType {
public:
  static std::string unknown();
  static std::string result();
  static std::string keyLabel();
};

class ErrorTextRecordKey {
public:
  static std::string number(llvm::StringRef type);
  static std::string numberIn(llvm::StringRef type);
};

class ErrorTextDictMember {
public:
  static std::string callOnly(llvm::StringRef text, llvm::StringRef name);
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type, llvm::StringRef text);
};

class ErrorTextDictIndex {
public:
  static std::string mapIndex(llvm::StringRef text);
  static std::string setIndex(llvm::StringRef text);
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

class ErrorTextDictDelete {
public:
  static std::string entryPart(bool isMap);
  static std::string property(llvm::StringRef kind, llvm::StringRef text, llvm::StringRef what);
};

class ErrorTextDictIn {
public:
  static std::string undefined();
  static std::string property(llvm::StringRef kind, llvm::StringRef text, llvm::StringRef what);
};

class ErrorTextPairParts {
public:
  static std::string keysDiffer(llvm::StringRef type, llvm::StringRef other);
  static std::string valuesDiffer(llvm::StringRef type, llvm::StringRef other);

private:
  static std::string differ(llvm::StringRef parts, llvm::StringRef type, llvm::StringRef other);
};

class ErrorTextPairsKnown {
public:
  static std::string undefined();
};

class ErrorTextDictSource {
public:
  static std::string notIter(llvm::StringRef what, llvm::StringRef type, bool record);
  static std::string notPairs(llvm::StringRef what, llvm::StringRef wanted, llvm::StringRef given,
                              bool array);
  static std::string values(llvm::StringRef what, llvm::StringRef wanted, llvm::StringRef given);
  static std::string newName(llvm::StringRef name);
};

class ErrorTextDictArgs {
public:
  static std::string oneArg(llvm::StringRef name, bool isMap);
};

class ErrorTextDictTypes {
public:
  static std::string needsMap();
  static std::string needsSet();
  static std::string undefinedArg(llvm::StringRef name, bool isMap);
};

class ErrorTextDictPairs {
public:
  static std::string pairs(llvm::StringRef element);
};

class ErrorTextDictElements {
public:
  static std::string undefined(llvm::StringRef name);
};

}
