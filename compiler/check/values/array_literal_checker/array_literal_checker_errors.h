#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ArrayLiteralCheckerElementGivenErrorCode {
  HOLE_IN_ARRAY,
};

struct ArrayLiteralCheckerElementGivenError {
  ArrayLiteralCheckerElementGivenErrorCode code;
  std::string message;
};

class ErrorTextArrayHole {
public:
  static std::string hole();
};

enum class ArrayLiteralCheckerTupleSpreadErrorCode {
  ELEMENTS_DIFFER,
};

struct ArrayLiteralCheckerTupleSpreadError {
  ArrayLiteralCheckerTupleSpreadErrorCode code;
  std::string message;
};

class ErrorTextTupleSpread {
public:
  static std::string differ(llvm::StringRef type);
};

enum class ArrayLiteralCheckerSpreadableErrorCode {
  NOT_SPREADABLE,
};

struct ArrayLiteralCheckerSpreadableError {
  ArrayLiteralCheckerSpreadableErrorCode code;
  std::string message;
};

class ErrorTextSpreadable {
public:
  static std::string notSpread(llvm::StringRef type, bool record);
};

enum class ArrayLiteralCheckerNotEmptyErrorCode {
  EMPTY_ARRAY,
};

struct ArrayLiteralCheckerNotEmptyError {
  ArrayLiteralCheckerNotEmptyErrorCode code;
  std::string message;
};

class ErrorTextArrayEmpty {
public:
  static std::string empty();
};

enum class ArrayLiteralCheckerTypesJoinErrorCode {
  ELEMENT_TYPES_DIFFER,
};

struct ArrayLiteralCheckerTypesJoinError {
  ArrayLiteralCheckerTypesJoinErrorCode code;
  std::string message;
};

class ErrorTextArrayTypes {
public:
  static std::string differ(llvm::StringRef element, llvm::StringRef other);
  static std::string literals();
  static std::string tuple(llvm::StringRef types);
};

enum class ArrayLiteralCheckerElementKnownErrorCode {
  ONLY_UNDEFINED,
};

struct ArrayLiteralCheckerElementKnownError {
  ArrayLiteralCheckerElementKnownErrorCode code;
  std::string message;
};

class ErrorTextArrayElement {
public:
  static std::string alone(llvm::StringRef element);
};

enum class ArrayLiteralCheckerSpreadFitsErrorCode {
  SPREAD_MISMATCH,
};

struct ArrayLiteralCheckerSpreadFitsError {
  ArrayLiteralCheckerSpreadFitsErrorCode code;
  std::string message;
};

class ErrorTextSpreadFits {
public:
  static std::string into(llvm::StringRef from, llvm::StringRef to);
};

}
