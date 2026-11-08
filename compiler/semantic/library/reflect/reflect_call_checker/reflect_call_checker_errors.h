#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ReflectCallCheckerKeyOfRecordsErrorCode {
  NOT_A_KEY,
};

struct ReflectCallCheckerKeyOfRecordsError {
  ReflectCallCheckerKeyOfRecordsErrorCode code;
  std::string message;
};

enum class ReflectCallCheckerTargetObjectErrorCode {
  NOT_AN_OBJECT,
};

struct ReflectCallCheckerTargetObjectError {
  ReflectCallCheckerTargetObjectErrorCode code;
  std::string message;
};

enum class ReflectCallCheckerArgumentListErrorCode {
  TUPLE,
  NOT_AN_ARRAY,
};

struct ReflectCallCheckerArgumentListError {
  ReflectCallCheckerArgumentListErrorCode code;
  std::string message;
};

class ErrorTextReflectCall {
public:
  static std::string decorator();
  static std::string keys(llvm::StringRef fn);
  static std::string unknown(llvm::StringRef fn);
};

class ErrorTextMetadataKey {
public:
  static std::string type(llvm::StringRef type);
  static std::string design();
};

class ErrorTextMetadataValue {
public:
  static std::string voidValue();
  static std::string label(llvm::StringRef label);
  static std::string since(llvm::StringRef label, llvm::StringRef type);
  static std::string undefined(llvm::StringRef label);
};

class ErrorTextMetadataTarget {
public:
  static std::string notObject(llvm::StringRef type);
};

class ErrorTextReflectArgs {
public:
  static std::string tuple();
  static std::string notArray(llvm::StringRef type);
};

}
