#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class NewCheckerNamespaceNewErrorCode {
  NAMESPACE_CLASS,
};

struct NewCheckerNamespaceNewError {
  NewCheckerNamespaceNewErrorCode code;
  std::string message;
};

class ErrorTextNamespaceNew {
public:
  static std::string namespaced(llvm::StringRef name, llvm::StringRef member);
};

enum class NewCheckerCryptoNewErrorCode {
  CRYPTO_KEY,
  SUBTLE_CRYPTO,
};

struct NewCheckerCryptoNewError {
  NewCheckerCryptoNewErrorCode code;
  std::string message;
};

class ErrorTextCryptoNew {
public:
  static std::string cryptoKey();
  static std::string subtle();
};

enum class NewCheckerSupportedNewErrorCode {
  UNSUPPORTED,
};

struct NewCheckerSupportedNewError {
  NewCheckerSupportedNewErrorCode code;
  std::string message;
};

class ErrorTextSupportedNew {
public:
  static std::string unsupported(llvm::StringRef name, llvm::StringRef why);
};

enum class NewCheckerNoTypeArgsErrorCode {
  TYPE_ARGUMENTS_GIVEN,
};

struct NewCheckerNoTypeArgsError {
  NewCheckerNoTypeArgsErrorCode code;
  std::string message;
};

class ErrorTextNoTypeArgs {
public:
  static std::string given(llvm::StringRef name);
};

enum class NewCheckerClassFoundErrorCode {
  CLASS_NOT_FOUND,
};

struct NewCheckerClassFoundError {
  NewCheckerClassFoundErrorCode code;
  std::string message;
};

class ErrorTextClassFound {
public:
  static std::string notFound(llvm::StringRef name);
};

enum class NewCheckerArrayElementErrorCode {
  TOO_MANY_TYPE_ARGUMENTS,
  NEEDS_ELEMENT_TYPE,
};

struct NewCheckerArrayElementError {
  NewCheckerArrayElementErrorCode code;
  std::string message;
};

class ErrorTextNewArray {
public:
  static std::string oneArg();
  static std::string needs();
};

enum class NewCheckerArrayArgsErrorCode {
  VALUES_NEED_LITERAL,
};

struct NewCheckerArrayArgsError {
  NewCheckerArrayArgsErrorCode code;
  std::string message;
};

class ErrorTextArrayArgs {
public:
  static std::string literal();
};

enum class NewCheckerZeroValueErrorCode {
  NO_ZERO_VALUE,
};

struct NewCheckerZeroValueError {
  NewCheckerZeroValueErrorCode code;
  std::string message;
};

class ErrorTextZeroValue {
public:
  static std::string none(llvm::StringRef element);
};

}
