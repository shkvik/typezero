#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class DeclarationFilesRegistryExportFoundErrorCode {
  NOT_EXPORTED,
};

struct DeclarationFilesRegistryExportFoundError {
  DeclarationFilesRegistryExportFoundErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryExportDeclaredErrorCode {
  NOT_DECLARED,
};

struct DeclarationFilesRegistryExportDeclaredError {
  DeclarationFilesRegistryExportDeclaredErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryCheckAliasErrorCode {
  NOT_A_TYPE,
  TYPE_PARAMS_DIFFER,
  FIELD_ONLY_HERE,
  FIELD_TYPE_DIFFERS,
  READONLY_DIFFERS,
  FIELD_ONLY_THERE,
  TYPES_DIFFER,
};

struct DeclarationFilesRegistryCheckAliasError {
  DeclarationFilesRegistryCheckAliasErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryParamCountErrorCode {
  COUNTS_DIFFER,
};

struct DeclarationFilesRegistryParamCountError {
  DeclarationFilesRegistryParamCountErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryParamShapeErrorCode {
  SHAPE_DIFFERS,
};

struct DeclarationFilesRegistryParamShapeError {
  DeclarationFilesRegistryParamShapeErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryStoredTypeErrorCode {
  TYPES_DIFFER,
};

struct DeclarationFilesRegistryStoredTypeError {
  DeclarationFilesRegistryStoredTypeErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryFunctionFoundErrorCode {
  NOT_A_FUNCTION,
};

struct DeclarationFilesRegistryFunctionFoundError {
  DeclarationFilesRegistryFunctionFoundErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryTypeParamCountErrorCode {
  COUNTS_DIFFER,
};

struct DeclarationFilesRegistryTypeParamCountError {
  DeclarationFilesRegistryTypeParamCountErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryReturnWrittenErrorCode {
  NOT_WRITTEN,
};

struct DeclarationFilesRegistryReturnWrittenError {
  DeclarationFilesRegistryReturnWrittenErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistrySameReturnErrorCode {
  TYPES_DIFFER,
};

struct DeclarationFilesRegistrySameReturnError {
  DeclarationFilesRegistrySameReturnErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryMethodSignatureErrorCode {
  IS_A_METHOD,
};

struct DeclarationFilesRegistryMethodSignatureError {
  DeclarationFilesRegistryMethodSignatureErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryDeclaredFieldErrorCode {
  IS_A_FIELD,
};

struct DeclarationFilesRegistryDeclaredFieldError {
  DeclarationFilesRegistryDeclaredFieldErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryReadonlyAgreesErrorCode {
  READONLY_DIFFERS,
};

struct DeclarationFilesRegistryReadonlyAgreesError {
  DeclarationFilesRegistryReadonlyAgreesErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryOptionalAgreesErrorCode {
  OPTIONAL_DIFFERS,
};

struct DeclarationFilesRegistryOptionalAgreesError {
  DeclarationFilesRegistryOptionalAgreesErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryClassFoundErrorCode {
  NOT_A_CLASS,
};

struct DeclarationFilesRegistryClassFoundError {
  DeclarationFilesRegistryClassFoundErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryAccessWrittenErrorCode {
  NOT_WRITTEN,
};

struct DeclarationFilesRegistryAccessWrittenError {
  DeclarationFilesRegistryAccessWrittenErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryCallableConstructorErrorCode {
  NO_CONSTRUCTOR,
};

struct DeclarationFilesRegistryCallableConstructorError {
  DeclarationFilesRegistryCallableConstructorErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryConstructorAccessErrorCode {
  ACCESS_DIFFERS,
};

struct DeclarationFilesRegistryConstructorAccessError {
  DeclarationFilesRegistryConstructorAccessErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryMemberFoundErrorCode {
  NO_MEMBER,
};

struct DeclarationFilesRegistryMemberFoundError {
  DeclarationFilesRegistryMemberFoundErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryMemberVisibleErrorCode {
  PRIVATE_MEMBER,
};

struct DeclarationFilesRegistryMemberVisibleError {
  DeclarationFilesRegistryMemberVisibleErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryAccessAgreesErrorCode {
  ACCESS_DIFFERS,
};

struct DeclarationFilesRegistryAccessAgreesError {
  DeclarationFilesRegistryAccessAgreesErrorCode code;
  std::string message;
};

enum class DeclarationFilesRegistryMemberListedErrorCode {
  NOT_DECLARED,
};

struct DeclarationFilesRegistryMemberListedError {
  DeclarationFilesRegistryMemberListedErrorCode code;
  std::string message;
};

class ErrorTextExports {
public:
  static std::string unexported(llvm::StringRef name, llvm::StringRef file);
  static std::string undeclared(llvm::StringRef name, llvm::StringRef declarations);
};

class ErrorTextDeclaration {
public:
  static std::string quoted(llvm::StringRef name);
  static std::string memberOf(llvm::StringRef name, llvm::StringRef cls);
  static std::string fieldOf(llvm::StringRef field, llvm::StringRef what);
  static std::string paramOf(llvm::StringRef param, llvm::StringRef what);
  static std::string typeOf(llvm::StringRef what);
  static std::string gives(llvm::StringRef what);
  static std::string notType(llvm::StringRef name, llvm::StringRef file);
  static std::string typeParams(llvm::StringRef name, size_t here, size_t there,
                                llvm::StringRef file);
  static std::string extraField(llvm::StringRef what, llvm::StringRef field, llvm::StringRef file);
  static std::string absentHere(llvm::StringRef what, llvm::StringRef field, llvm::StringRef file);
  static std::string readonly(llvm::StringRef what, bool readonly, llvm::StringRef file);
  static std::string optional(llvm::StringRef what, bool optional, llvm::StringRef file);
  static std::string mismatch(llvm::StringRef what, llvm::StringRef here, llvm::StringRef there,
                              llvm::StringRef file);
  static std::string paramCount(llvm::StringRef what, size_t here, size_t there,
                                llvm::StringRef file);
  static std::string paramKind(llvm::StringRef param, bool optional, llvm::StringRef file);
  static std::string noFunction(llvm::StringRef name, llvm::StringRef file);
  static std::string otherCount(llvm::StringRef what, llvm::StringRef file);
  static std::string returnType(llvm::StringRef what);
};

class ErrorTextDeclaredClass {
public:
  static std::string notClass(llvm::StringRef name, llvm::StringRef file);
  static std::string noAccess(llvm::StringRef what);
  static std::string isField(llvm::StringRef what, llvm::StringRef file);
  static std::string isMethod(llvm::StringRef what, llvm::StringRef file);
  static std::string noCallable(llvm::StringRef cls, llvm::StringRef file);
  static std::string newAccess(llvm::StringRef cls, llvm::StringRef access, llvm::StringRef file);
  static std::string noMember(llvm::StringRef cls, llvm::StringRef name, bool isStatic,
                              llvm::StringRef file);
  static std::string isPrivate(llvm::StringRef what, llvm::StringRef file);
  static std::string accessIs(llvm::StringRef what, llvm::StringRef access, llvm::StringRef file);
  static std::string undeclared(llvm::StringRef name, llvm::StringRef cls, bool isStatic,
                                llvm::StringRef declarations);
};

}
