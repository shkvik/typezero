#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class DictNewCheckerPartsJoinErrorCode {
  KEYS_DIFFER,
  VALUES_DIFFER,
};

struct DictNewCheckerPartsJoinError {
  DictNewCheckerPartsJoinErrorCode code;
  std::string message;
};

class ErrorTextPairParts {
public:
  static std::string differ(bool keys, llvm::StringRef type, llvm::StringRef other);
};

enum class DictNewCheckerPairsKnownErrorCode {
  TYPES_UNDEFINED,
};

struct DictNewCheckerPairsKnownError {
  DictNewCheckerPairsKnownErrorCode code;
  std::string message;
};

class ErrorTextPairsKnown {
public:
  static std::string undefined();
};

enum class DictNewCheckerSourceShapeErrorCode {
  NOT_ITERABLE,
  NOT_PAIRS,
  WRONG_VALUES,
};

struct DictNewCheckerSourceShapeError {
  DictNewCheckerSourceShapeErrorCode code;
  std::string message;
};

class ErrorTextDictSource {
public:
  static std::string notIter(llvm::StringRef what, llvm::StringRef type, bool record);
  static std::string notPairs(llvm::StringRef what, llvm::StringRef wanted, llvm::StringRef given,
                              bool array);
  static std::string values(llvm::StringRef what, llvm::StringRef wanted, llvm::StringRef given);
};

enum class DictNewCheckerArgumentCountErrorCode {
  TOO_MANY_ARGUMENTS,
};

struct DictNewCheckerArgumentCountError {
  DictNewCheckerArgumentCountErrorCode code;
  std::string message;
};

class ErrorTextDictArgs {
public:
  static std::string oneArg(llvm::StringRef name, bool isMap);
};

enum class DictNewCheckerTypesGivenErrorCode {
  MAP_NEEDS_TYPES,
  SET_NEEDS_TYPES,
  UNDEFINED_NEEDS_TYPES,
};

struct DictNewCheckerTypesGivenError {
  DictNewCheckerTypesGivenErrorCode code;
  std::string message;
};

class ErrorTextDictTypes {
public:
  static std::string needsMap();
  static std::string needsSet();
  static std::string undefinedArg(llvm::StringRef name, bool isMap);
};

enum class DictNewCheckerPairShapeErrorCode {
  NOT_PAIRS,
};

struct DictNewCheckerPairShapeError {
  DictNewCheckerPairShapeErrorCode code;
  std::string message;
};

class ErrorTextDictPairs {
public:
  static std::string pairs(llvm::StringRef element);
};

enum class DictNewCheckerElementsKnownErrorCode {
  TYPES_UNDEFINED,
};

struct DictNewCheckerElementsKnownError {
  DictNewCheckerElementsKnownErrorCode code;
  std::string message;
};

class ErrorTextDictElements {
public:
  static std::string undefined(llvm::StringRef name);
};

}
