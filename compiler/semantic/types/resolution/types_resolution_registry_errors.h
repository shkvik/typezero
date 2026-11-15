#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TypesResolutionRegistryStandaloneErrorCode {
  VOID_TYPE,
  UNDEFINED_ALONE,
};

struct TypesResolutionRegistryStandaloneError {
  TypesResolutionRegistryStandaloneErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryEnumMemberErrorCode {
  NO_SUCH_MEMBER,
};

struct TypesResolutionRegistryEnumMemberError {
  TypesResolutionRegistryEnumMemberErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryIntlTypeErrorCode {
  NOT_FOUND,
};

struct TypesResolutionRegistryIntlTypeError {
  TypesResolutionRegistryIntlTypeErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryBufferArgumentErrorCode {
  NOT_ARRAY_BUFFER,
};

struct TypesResolutionRegistryBufferArgumentError {
  TypesResolutionRegistryBufferArgumentErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryArrayArgumentErrorCode {
  NOT_ONE_ARGUMENT,
};

struct TypesResolutionRegistryArrayArgumentError {
  TypesResolutionRegistryArrayArgumentErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryClassGenericErrorCode {
  NOT_GENERIC,
};

struct TypesResolutionRegistryClassGenericError {
  TypesResolutionRegistryClassGenericErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryAliasGenericErrorCode {
  NOT_GENERIC,
};

struct TypesResolutionRegistryAliasGenericError {
  TypesResolutionRegistryAliasGenericErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryDescriptorArityErrorCode {
  NOT_TWO_ARGUMENTS,
};

struct TypesResolutionRegistryDescriptorArityError {
  TypesResolutionRegistryDescriptorArityErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryDescriptorParamsErrorCode {
  NOT_A_LIST,
};

struct TypesResolutionRegistryDescriptorParamsError {
  TypesResolutionRegistryDescriptorParamsErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryClassInstanceErrorCode {
  NOT_A_CLASS,
};

struct TypesResolutionRegistryClassInstanceError {
  TypesResolutionRegistryClassInstanceErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryArrayElementErrorCode {
  NOT_SUPPORTED,
};

struct TypesResolutionRegistryArrayElementError {
  TypesResolutionRegistryArrayElementErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryUnionPartErrorCode {
  VOID_MEMBER,
};

struct TypesResolutionRegistryUnionPartError {
  TypesResolutionRegistryUnionPartErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryUnionAllowedErrorCode {
  NULL_MEMBER,
  NOT_SUPPORTED,
};

struct TypesResolutionRegistryUnionAllowedError {
  TypesResolutionRegistryUnionAllowedErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryInferBindingErrorCode {
  OUTSIDE_EXTENDS,
};

struct TypesResolutionRegistryInferBindingError {
  TypesResolutionRegistryInferBindingErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryRestParamErrorCode {
  NOT_AN_ARRAY,
};

struct TypesResolutionRegistryRestParamError {
  TypesResolutionRegistryRestParamErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryParamOrderErrorCode {
  REQUIRED_AFTER_OPTIONAL,
};

struct TypesResolutionRegistryParamOrderError {
  TypesResolutionRegistryParamOrderErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryFixedLengthErrorCode {
  REST_ELEMENT,
};

struct TypesResolutionRegistryFixedLengthError {
  TypesResolutionRegistryFixedLengthErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryTupleElementErrorCode {
  TYPE_LIST,
};

struct TypesResolutionRegistryTupleElementError {
  TypesResolutionRegistryTupleElementErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistrySpreadJoinedErrorCode {
  NOT_JOINED,
};

struct TypesResolutionRegistrySpreadJoinedError {
  TypesResolutionRegistrySpreadJoinedErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryOverloadAllowedErrorCode {
  NOT_AN_INTERFACE,
};

struct TypesResolutionRegistryOverloadAllowedError {
  TypesResolutionRegistryOverloadAllowedErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryNewSignatureErrorCode {
  DUPLICATE_SIGNATURE,
};

struct TypesResolutionRegistryNewSignatureError {
  TypesResolutionRegistryNewSignatureErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryNewFieldErrorCode {
  DUPLICATE_FIELD,
};

struct TypesResolutionRegistryNewFieldError {
  TypesResolutionRegistryNewFieldErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryResolveTypeOpErrorCode {
  NOT_AN_OBJECT_TYPE,
  NO_SUCH_MEMBER,
  TYPE_OF_UNKNOWN,
  NO_SUCH_FIELD,
  RENAME_NOT_STRING,
  KEYS_NOT_STRINGS,
};

struct TypesResolutionRegistryResolveTypeOpError {
  TypesResolutionRegistryResolveTypeOpErrorCode code;
  std::string message;
};

enum class TypesResolutionRegistryResolveUtilityTypeErrorCode {
  NOT_A_UTILITY,
  WRONG_ARGUMENT_COUNT,
  NOT_A_FUNCTION_TYPE,
  NOT_A_STRING_TYPE,
};

struct TypesResolutionRegistryResolveUtilityTypeError {
  TypesResolutionRegistryResolveUtilityTypeErrorCode code;
  std::string message;
};

class ErrorTextTypeUse {
public:
  static std::string voidType();
  static std::string undefined();
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

class ErrorTextTypeFields {
public:
  static std::string signatures(llvm::StringRef name);
  static std::string duplicate(llvm::StringRef type, llvm::StringRef name);
  static std::string sameField(llvm::StringRef name);
};

class ErrorTextUtility {
public:
  static std::string count(llvm::StringRef name, int count);
  static std::string noFunction(llvm::StringRef name, llvm::StringRef type);
  static std::string notString(llvm::StringRef name, llvm::StringRef type);
};

}
