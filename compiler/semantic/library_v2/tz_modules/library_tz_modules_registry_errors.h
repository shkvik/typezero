#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>

namespace tz {

enum class LibraryTzModulesRegistryPlainSignatureErrorCode {
  TYPE_PARAMS_OR_REST,
};

struct LibraryTzModulesRegistryPlainSignatureError {
  LibraryTzModulesRegistryPlainSignatureErrorCode code;
  std::string message;
};

enum class LibraryTzModulesRegistryExternalParamErrorCode {
  OPTIONAL_PARAM,
  PARAM_TYPE,
};

struct LibraryTzModulesRegistryExternalParamError {
  LibraryTzModulesRegistryExternalParamErrorCode code;
  std::string message;
};

enum class LibraryTzModulesRegistryExternalReturnErrorCode {
  RETURN_TYPE,
};

struct LibraryTzModulesRegistryExternalReturnError {
  LibraryTzModulesRegistryExternalReturnErrorCode code;
  std::string message;
};

class ErrorTextIntrinsic {
public:
  static std::string plain();
  static std::string optional();
  static std::string takes(llvm::StringRef type);
  static std::string gives(llvm::StringRef type);
};

enum class LibraryTzModulesRegistryHeaderNameErrorCode {
  NOT_A_TOKEN,
};

struct LibraryTzModulesRegistryHeaderNameError {
  LibraryTzModulesRegistryHeaderNameErrorCode code;
  std::string message;
};

enum class LibraryTzModulesRegistryHeaderTextErrorCode {
  LINE_BREAK,
};

struct LibraryTzModulesRegistryHeaderTextError {
  LibraryTzModulesRegistryHeaderTextErrorCode code;
  std::string message;
};

enum class LibraryTzModulesRegistryRequestUrlErrorCode {
  INVALID_URL,
  CREDENTIALS,
};

struct LibraryTzModulesRegistryRequestUrlError {
  LibraryTzModulesRegistryRequestUrlErrorCode code;
  std::string message;
};

enum class LibraryTzModulesRegistryMethodNameErrorCode {
  NOT_A_TOKEN,
  FORBIDDEN,
};

struct LibraryTzModulesRegistryMethodNameError {
  LibraryTzModulesRegistryMethodNameErrorCode code;
  std::string message;
};

class ErrorTextHeader {
public:
  static std::string name(llvm::StringRef name);
  static std::string value();
};

class ErrorTextHttpResponse {
public:
  static std::string noBody(int64_t status);
  static std::string statusText();
};

class ErrorTextHttpRequest {
public:
  static std::string badUrl(llvm::StringRef text);
  static std::string userInUrl();
  static std::string unknown();
  static std::string badMethod(llvm::StringRef method);
  static std::string forbidden(llvm::StringRef method);
  static std::string getBody(llvm::StringRef method, bool given);
};

enum class LibraryTzModulesRegistryKnownBaseErrorCode {
  UNKNOWN_BASE,
};

struct LibraryTzModulesRegistryKnownBaseError {
  LibraryTzModulesRegistryKnownBaseErrorCode code;
  std::string message;
};

enum class LibraryTzModulesRegistryUrlTextErrorCode {
  INVALID_URL,
};

struct LibraryTzModulesRegistryUrlTextError {
  LibraryTzModulesRegistryUrlTextErrorCode code;
  std::string message;
};

enum class LibraryTzModulesRegistryKnownInputErrorCode {
  UNKNOWN_INPUT,
};

struct LibraryTzModulesRegistryKnownInputError {
  LibraryTzModulesRegistryKnownInputErrorCode code;
  std::string message;
};

enum class LibraryTzModulesSetterValueErrorCode {
  NOT_A_URL,
  NOT_A_PROTOCOL,
  NOT_A_PORT,
  NOT_A_HOST,
};

struct LibraryTzModulesSetterValueError {
  LibraryTzModulesSetterValueErrorCode code;
  std::string message;
};

class ErrorTextUrlNew {
public:
  static std::string base();
  static std::string invalid(llvm::StringRef text, bool based);
  static std::string input(bool based);
};

class ErrorTextUrlSetter {
public:
  static std::string href(llvm::StringRef text);
  static std::string protocol(llvm::StringRef text);
  static std::string port(llvm::StringRef text);
  static std::string host(llvm::StringRef text, bool withPort);
};

}
