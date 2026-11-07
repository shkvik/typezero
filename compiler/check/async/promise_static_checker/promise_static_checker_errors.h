#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class PromiseStaticCheckerStaticTypeArgsErrorCode {
  TYPE_ARGUMENTS_UNUSED,
};

struct PromiseStaticCheckerStaticTypeArgsError {
  PromiseStaticCheckerStaticTypeArgsErrorCode code;
  std::string message;
};

class ErrorTextStaticTypeArgs {
public:
  static std::string resolvers();
};

enum class PromiseStaticCheckerNotVoidErrorCode {
  VOID_VALUE,
};

struct PromiseStaticCheckerNotVoidError {
  PromiseStaticCheckerNotVoidErrorCode code;
  std::string message;
};

class ErrorTextPromiseVoid {
public:
  static std::string given(llvm::StringRef fn);
};

enum class PromiseStaticCheckerResolverTypeArgsErrorCode {
  TOO_MANY_TYPE_ARGUMENTS,
};

struct PromiseStaticCheckerResolverTypeArgsError {
  PromiseStaticCheckerResolverTypeArgsErrorCode code;
  std::string message;
};

enum class PromiseStaticCheckerResolverValueErrorCode {
  VALUE_TYPE_UNKNOWN,
  ALREADY_REPORTED,
};

struct PromiseStaticCheckerResolverValueError {
  PromiseStaticCheckerResolverValueErrorCode code;
  std::string message;
};

class ErrorTextResolverValue {
public:
  static std::string typeArgs();
  static std::string unknown();
};

enum class PromiseStaticCheckerRaceJoinErrorCode {
  TYPES_DIFFER,
};

struct PromiseStaticCheckerRaceJoinError {
  PromiseStaticCheckerRaceJoinErrorCode code;
  std::string message;
};

class ErrorTextRaceJoin {
public:
  static std::string differ(llvm::StringRef fn, llvm::StringRef type, llvm::StringRef other);
};

enum class PromiseStaticCheckerAllValuesErrorCode {
  VOID_PROMISE,
};

struct PromiseStaticCheckerAllValuesError {
  PromiseStaticCheckerAllValuesErrorCode code;
  std::string message;
};

class ErrorTextAllValues {
public:
  static std::string voidValue();
};

enum class PromiseStaticCheckerEmptyListErrorCode {
  ELEMENT_TYPE_UNKNOWN,
};

struct PromiseStaticCheckerEmptyListError {
  PromiseStaticCheckerEmptyListErrorCode code;
  std::string message;
};

enum class PromiseStaticCheckerPlainValueErrorCode {
  MAY_BE_PROMISE,
};

struct PromiseStaticCheckerPlainValueError {
  PromiseStaticCheckerPlainValueErrorCode code;
  std::string message;
};

enum class PromiseStaticCheckerPromiseArrayErrorCode {
  NOT_AN_ARRAY,
};

struct PromiseStaticCheckerPromiseArrayError {
  PromiseStaticCheckerPromiseArrayErrorCode code;
  std::string message;
};

class ErrorTextPromiseList {
public:
  static std::string noType(llvm::StringRef fn);
  static std::string mayBe(llvm::StringRef fn, llvm::StringRef type);
  static std::string notArray(llvm::StringRef fn, llvm::StringRef type, bool iterable,
                              llvm::StringRef values);
};

class ErrorTextNoReject {
public:
  static std::string rejected();
  static std::string settled();
  static std::string any();
};

class ErrorTextPromiseStatic {
public:
  static std::string unknown(llvm::StringRef name);
};

}
