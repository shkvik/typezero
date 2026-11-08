#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TypeResolverStandaloneErrorCode {
  VOID_TYPE,
  UNDEFINED_ALONE,
};

struct TypeResolverStandaloneError {
  TypeResolverStandaloneErrorCode code;
  std::string message;
};

class ErrorTextTypeUse {
public:
  static std::string voidType();
  static std::string undefined();
};

enum class TypeResolverEnumMemberErrorCode {
  NO_SUCH_MEMBER,
};

struct TypeResolverEnumMemberError {
  TypeResolverEnumMemberErrorCode code;
  std::string message;
};

class ErrorTextTypeName {
public:
  static std::string enumMember(llvm::StringRef owner, llvm::StringRef member);
  static std::string notFound(llvm::StringRef name);
  static std::string arrayArg();
  static std::string resultArgs(llvm::StringRef name);
  static std::string valuesArg(llvm::StringRef name);
  static std::string promiseArg();
  static std::string nullType();
  static std::string longType();
  static std::string shared();
  static std::string bigintType();
  static std::string bigintName(llvm::StringRef name);
};

enum class TypeResolverIntlTypeErrorCode {
  NOT_FOUND,
};

struct TypeResolverIntlTypeError {
  TypeResolverIntlTypeErrorCode code;
  std::string message;
};

enum class TypeResolverBufferArgumentErrorCode {
  NOT_ARRAY_BUFFER,
};

struct TypeResolverBufferArgumentError {
  TypeResolverBufferArgumentErrorCode code;
  std::string message;
};

class ErrorTextGenericRef {
public:
  static std::string bufferArg(llvm::StringRef name);
  static std::string oneArg(llvm::StringRef name);
  static std::string plainClass(llvm::StringRef name);
  static std::string plainAlias(llvm::StringRef name);
  static std::string descriptor();
  static std::string paramList();
  static std::string instanceOf(llvm::StringRef type);
  static std::string noSupport(llvm::StringRef name);
};

enum class TypeResolverArrayArgumentErrorCode {
  NOT_ONE_ARGUMENT,
};

struct TypeResolverArrayArgumentError {
  TypeResolverArrayArgumentErrorCode code;
  std::string message;
};

enum class TypeResolverClassGenericErrorCode {
  NOT_GENERIC,
};

struct TypeResolverClassGenericError {
  TypeResolverClassGenericErrorCode code;
  std::string message;
};

enum class TypeResolverAliasGenericErrorCode {
  NOT_GENERIC,
};

struct TypeResolverAliasGenericError {
  TypeResolverAliasGenericErrorCode code;
  std::string message;
};

enum class TypeResolverDescriptorArityErrorCode {
  NOT_TWO_ARGUMENTS,
};

struct TypeResolverDescriptorArityError {
  TypeResolverDescriptorArityErrorCode code;
  std::string message;
};

enum class TypeResolverDescriptorParamsErrorCode {
  NOT_A_LIST,
};

struct TypeResolverDescriptorParamsError {
  TypeResolverDescriptorParamsErrorCode code;
  std::string message;
};

enum class TypeResolverClassInstanceErrorCode {
  NOT_A_CLASS,
};

struct TypeResolverClassInstanceError {
  TypeResolverClassInstanceErrorCode code;
  std::string message;
};

enum class TypeResolverArrayElementErrorCode {
  NOT_SUPPORTED,
};

struct TypeResolverArrayElementError {
  TypeResolverArrayElementErrorCode code;
  std::string message;
};

class ErrorTextTypeShape {
public:
  static std::string arrayOf(llvm::StringRef type);
  static std::string voidUnion();
  static std::string nullUnion();
  static std::string noUnion(llvm::StringRef type, llvm::StringRef problem);
  static std::string predicate(llvm::StringRef name);
  static std::string inferOnly();
  static std::string restArray();
  static std::string paramOrder();
  static std::string tupleRest();
  static std::string typeList(llvm::StringRef name);
};

enum class TypeResolverUnionPartErrorCode {
  VOID_MEMBER,
};

struct TypeResolverUnionPartError {
  TypeResolverUnionPartErrorCode code;
  std::string message;
};

enum class TypeResolverUnionAllowedErrorCode {
  NULL_MEMBER,
  NOT_SUPPORTED,
};

struct TypeResolverUnionAllowedError {
  TypeResolverUnionAllowedErrorCode code;
  std::string message;
};

enum class TypeResolverInferBindingErrorCode {
  OUTSIDE_EXTENDS,
};

struct TypeResolverInferBindingError {
  TypeResolverInferBindingErrorCode code;
  std::string message;
};

enum class TypeResolverRestParamErrorCode {
  NOT_AN_ARRAY,
};

struct TypeResolverRestParamError {
  TypeResolverRestParamErrorCode code;
  std::string message;
};

enum class TypeResolverParamOrderErrorCode {
  REQUIRED_AFTER_OPTIONAL,
};

struct TypeResolverParamOrderError {
  TypeResolverParamOrderErrorCode code;
  std::string message;
};

enum class TypeResolverFixedLengthErrorCode {
  REST_ELEMENT,
};

struct TypeResolverFixedLengthError {
  TypeResolverFixedLengthErrorCode code;
  std::string message;
};

enum class TypeResolverTupleElementErrorCode {
  TYPE_LIST,
};

struct TypeResolverTupleElementError {
  TypeResolverTupleElementErrorCode code;
  std::string message;
};

enum class TypeResolverSpreadJoinedErrorCode {
  NOT_JOINED,
};

struct TypeResolverSpreadJoinedError {
  TypeResolverSpreadJoinedErrorCode code;
  std::string message;
};

enum class TypeResolverOverloadAllowedErrorCode {
  NOT_AN_INTERFACE,
};

struct TypeResolverOverloadAllowedError {
  TypeResolverOverloadAllowedErrorCode code;
  std::string message;
};

class ErrorTextTypeFields {
public:
  static std::string signatures(llvm::StringRef name);
  static std::string duplicate(llvm::StringRef type, llvm::StringRef name);
  static std::string sameField(llvm::StringRef name);
};

enum class TypeResolverNewSignatureErrorCode {
  DUPLICATE_SIGNATURE,
};

struct TypeResolverNewSignatureError {
  TypeResolverNewSignatureErrorCode code;
  std::string message;
};

enum class TypeResolverNewFieldErrorCode {
  DUPLICATE_FIELD,
};

struct TypeResolverNewFieldError {
  TypeResolverNewFieldErrorCode code;
  std::string message;
};

}
