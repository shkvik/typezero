#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class DeclarationCheckerExportFoundErrorCode {
  NOT_EXPORTED,
};

struct DeclarationCheckerExportFoundError {
  DeclarationCheckerExportFoundErrorCode code;
  std::string message;
};

class ErrorTextExports {
public:
  static std::string unexported(llvm::StringRef name, llvm::StringRef file);
  static std::string undeclared(llvm::StringRef name, llvm::StringRef declarations);
};

enum class DeclarationCheckerExportDeclaredErrorCode {
  NOT_DECLARED,
};

struct DeclarationCheckerExportDeclaredError {
  DeclarationCheckerExportDeclaredErrorCode code;
  std::string message;
};

enum class DeclarationCheckerCheckAliasErrorCode {
  NOT_A_TYPE,
  TYPE_PARAMS_DIFFER,
  FIELD_ONLY_HERE,
  FIELD_TYPE_DIFFERS,
  READONLY_DIFFERS,
  FIELD_ONLY_THERE,
  TYPES_DIFFER,
};

struct DeclarationCheckerCheckAliasError {
  DeclarationCheckerCheckAliasErrorCode code;
  std::string message;
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

enum class DeclarationCheckerParamCountErrorCode {
  COUNTS_DIFFER,
};

struct DeclarationCheckerParamCountError {
  DeclarationCheckerParamCountErrorCode code;
  std::string message;
};

enum class DeclarationCheckerParamShapeErrorCode {
  SHAPE_DIFFERS,
};

struct DeclarationCheckerParamShapeError {
  DeclarationCheckerParamShapeErrorCode code;
  std::string message;
};

enum class DeclarationCheckerStoredTypeErrorCode {
  TYPES_DIFFER,
};

struct DeclarationCheckerStoredTypeError {
  DeclarationCheckerStoredTypeErrorCode code;
  std::string message;
};

enum class DeclarationCheckerFunctionFoundErrorCode {
  NOT_A_FUNCTION,
};

struct DeclarationCheckerFunctionFoundError {
  DeclarationCheckerFunctionFoundErrorCode code;
  std::string message;
};

enum class DeclarationCheckerTypeParamCountErrorCode {
  COUNTS_DIFFER,
};

struct DeclarationCheckerTypeParamCountError {
  DeclarationCheckerTypeParamCountErrorCode code;
  std::string message;
};

enum class DeclarationCheckerReturnWrittenErrorCode {
  NOT_WRITTEN,
};

struct DeclarationCheckerReturnWrittenError {
  DeclarationCheckerReturnWrittenErrorCode code;
  std::string message;
};

enum class DeclarationCheckerSameReturnErrorCode {
  TYPES_DIFFER,
};

struct DeclarationCheckerSameReturnError {
  DeclarationCheckerSameReturnErrorCode code;
  std::string message;
};

enum class DeclarationCheckerMethodSignatureErrorCode {
  IS_A_METHOD,
};

struct DeclarationCheckerMethodSignatureError {
  DeclarationCheckerMethodSignatureErrorCode code;
  std::string message;
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

enum class DeclarationCheckerDeclaredFieldErrorCode {
  IS_A_FIELD,
};

struct DeclarationCheckerDeclaredFieldError {
  DeclarationCheckerDeclaredFieldErrorCode code;
  std::string message;
};

enum class DeclarationCheckerReadonlyAgreesErrorCode {
  READONLY_DIFFERS,
};

struct DeclarationCheckerReadonlyAgreesError {
  DeclarationCheckerReadonlyAgreesErrorCode code;
  std::string message;
};

enum class DeclarationCheckerOptionalAgreesErrorCode {
  OPTIONAL_DIFFERS,
};

struct DeclarationCheckerOptionalAgreesError {
  DeclarationCheckerOptionalAgreesErrorCode code;
  std::string message;
};

enum class DeclarationCheckerClassFoundErrorCode {
  NOT_A_CLASS,
};

struct DeclarationCheckerClassFoundError {
  DeclarationCheckerClassFoundErrorCode code;
  std::string message;
};

enum class DeclarationCheckerAccessWrittenErrorCode {
  NOT_WRITTEN,
};

struct DeclarationCheckerAccessWrittenError {
  DeclarationCheckerAccessWrittenErrorCode code;
  std::string message;
};

enum class DeclarationCheckerCallableConstructorErrorCode {
  NO_CONSTRUCTOR,
};

struct DeclarationCheckerCallableConstructorError {
  DeclarationCheckerCallableConstructorErrorCode code;
  std::string message;
};

enum class DeclarationCheckerConstructorAccessErrorCode {
  ACCESS_DIFFERS,
};

struct DeclarationCheckerConstructorAccessError {
  DeclarationCheckerConstructorAccessErrorCode code;
  std::string message;
};

enum class DeclarationCheckerMemberFoundErrorCode {
  NO_MEMBER,
};

struct DeclarationCheckerMemberFoundError {
  DeclarationCheckerMemberFoundErrorCode code;
  std::string message;
};

enum class DeclarationCheckerMemberVisibleErrorCode {
  PRIVATE_MEMBER,
};

struct DeclarationCheckerMemberVisibleError {
  DeclarationCheckerMemberVisibleErrorCode code;
  std::string message;
};

enum class DeclarationCheckerAccessAgreesErrorCode {
  ACCESS_DIFFERS,
};

struct DeclarationCheckerAccessAgreesError {
  DeclarationCheckerAccessAgreesErrorCode code;
  std::string message;
};

enum class DeclarationCheckerMemberListedErrorCode {
  NOT_DECLARED,
};

struct DeclarationCheckerMemberListedError {
  DeclarationCheckerMemberListedErrorCode code;
  std::string message;
};

}
