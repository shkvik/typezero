#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeCallsRegistryCheckValueCallErrorCode {
  TAKES_THIS,
};

struct CodeCallsRegistryCheckValueCallError {
  CodeCallsRegistryCheckValueCallErrorCode code;
  std::string message;
};

enum class CodeCallsRegistryNoTypeArgsErrorCode {
  TYPE_ARGUMENTS_GIVEN,
};

struct CodeCallsRegistryNoTypeArgsError {
  CodeCallsRegistryNoTypeArgsErrorCode code;
  std::string message;
};

enum class CodeCallsRegistryDirectTypeArgsErrorCode {
  NOT_GENERIC,
};

struct CodeCallsRegistryDirectTypeArgsError {
  CodeCallsRegistryDirectTypeArgsErrorCode code;
  std::string message;
};

enum class CodeCallsRegistryCalleeCallableErrorCode {
  MAY_BE_UNDEFINED,
  ALREADY_REPORTED,
  NOT_A_FUNCTION,
  NOT_CALLABLE,
};

struct CodeCallsRegistryCalleeCallableError {
  CodeCallsRegistryCalleeCallableErrorCode code;
  std::string message;
};

enum class CodeCallsRegistrySpreadFreeErrorCode {
  SPREAD_ARGUMENT,
};

struct CodeCallsRegistrySpreadFreeError {
  CodeCallsRegistrySpreadFreeErrorCode code;
  std::string message;
};

enum class CodeCallsRegistryCheckArityErrorCode {
  WRONG_COUNT,
  WRONG_COUNT_TAGGED,
};

struct CodeCallsRegistryCheckArityError {
  CodeCallsRegistryCheckArityErrorCode code;
  std::string message;
};

enum class CodeCallsRegistrySpreadRestErrorCode {
  OTHER_TYPE,
};

struct CodeCallsRegistrySpreadRestError {
  CodeCallsRegistrySpreadRestErrorCode code;
  std::string message;
};

enum class CodeCallsRegistryTupleRestErrorCode {
  WRONG_COUNT,
};

struct CodeCallsRegistryTupleRestError {
  CodeCallsRegistryTupleRestErrorCode code;
  std::string message;
};

enum class CodeCallsRegistryNamespaceNewErrorCode {
  NAMESPACE_CLASS,
};

struct CodeCallsRegistryNamespaceNewError {
  CodeCallsRegistryNamespaceNewErrorCode code;
  std::string message;
};

enum class CodeCallsRegistryCryptoNewErrorCode {
  CRYPTO_KEY,
  SUBTLE_CRYPTO,
};

struct CodeCallsRegistryCryptoNewError {
  CodeCallsRegistryCryptoNewErrorCode code;
  std::string message;
};

enum class CodeCallsRegistrySupportedNewErrorCode {
  UNSUPPORTED,
};

struct CodeCallsRegistrySupportedNewError {
  CodeCallsRegistrySupportedNewErrorCode code;
  std::string message;
};

enum class CodeCallsRegistryClassFoundErrorCode {
  CLASS_NOT_FOUND,
};

struct CodeCallsRegistryClassFoundError {
  CodeCallsRegistryClassFoundErrorCode code;
  std::string message;
};

enum class CodeCallsRegistryArrayElementErrorCode {
  TOO_MANY_TYPE_ARGUMENTS,
  NEEDS_ELEMENT_TYPE,
};

struct CodeCallsRegistryArrayElementError {
  CodeCallsRegistryArrayElementErrorCode code;
  std::string message;
};

enum class CodeCallsRegistryArrayArgsErrorCode {
  VALUES_NEED_LITERAL,
};

struct CodeCallsRegistryArrayArgsError {
  CodeCallsRegistryArrayArgsErrorCode code;
  std::string message;
};

enum class CodeCallsRegistryZeroValueErrorCode {
  NO_ZERO_VALUE,
};

struct CodeCallsRegistryZeroValueError {
  CodeCallsRegistryZeroValueErrorCode code;
  std::string message;
};

class ErrorTextValueCall {
public:
  static std::string takesThis();
};

class ErrorTextCallTypeArgs {
public:
  static std::string forGeneric();
  static std::string notGeneric(llvm::StringRef name);
};

class ErrorTextCallee {
public:
  static std::string undefined();
  static std::string notFunc(llvm::StringRef name, llvm::StringRef type);
  static std::string uncallable(llvm::StringRef type);
};

class ErrorTextSpread {
public:
  static std::string notRest();
};

class ErrorTextCallMethod {
public:
  static std::string undefined(llvm::StringRef name);
  static std::string setsThis(llvm::StringRef name);
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type);
};

class ErrorTextCallName {
public:
  static std::string noSupport(llvm::StringRef name, llvm::StringRef why);
  static std::string notFunc(llvm::StringRef name);
};

class ErrorTextArity {
public:
  static std::string count(unsigned min, unsigned max, size_t got);
  static std::string tagged(unsigned min, unsigned max, size_t got);
  static std::string wanted(unsigned min, unsigned max);
};

class ErrorTextSpreadRest {
public:
  static std::string otherType(llvm::StringRef from, llvm::StringRef to);
};

class ErrorTextTupleRest {
public:
  static std::string count(size_t expected, size_t got);
};

class ErrorTextNamespaceNew {
public:
  static std::string namespaced(llvm::StringRef name, llvm::StringRef member);
};

class ErrorTextCryptoNew {
public:
  static std::string cryptoKey();
  static std::string subtle();
};

class ErrorTextSupportedNew {
public:
  static std::string unsupported(llvm::StringRef name, llvm::StringRef why);
};

class ErrorTextNoTypeArgs {
public:
  static std::string given(llvm::StringRef name);
};

class ErrorTextClassFound {
public:
  static std::string notFound(llvm::StringRef name);
};

class ErrorTextNewArray {
public:
  static std::string oneArg();
  static std::string needs();
  static std::string length();
};

class ErrorTextArrayArgs {
public:
  static std::string literal();
};

class ErrorTextZeroValue {
public:
  static std::string none(llvm::StringRef element);
};

class ErrorTextUnsupported {
public:
  static std::string why(llvm::StringRef name);
};

}
