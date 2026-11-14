#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class DeclarationClassesRegistryUniqueFieldErrorCode {
  DUPLICATE_FIELD,
};

struct DeclarationClassesRegistryUniqueFieldError {
  DeclarationClassesRegistryUniqueFieldErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryFieldOrMethodErrorCode {
  FIELD_AND_METHOD,
};

struct DeclarationClassesRegistryFieldOrMethodError {
  DeclarationClassesRegistryFieldOrMethodErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistrySetLaterErrorCode {
  SET_LATER,
};

struct DeclarationClassesRegistrySetLaterError {
  DeclarationClassesRegistrySetLaterErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryTypedFieldErrorCode {
  NO_TYPE,
  PROPERTY_NO_TYPE,
};

struct DeclarationClassesRegistryTypedFieldError {
  DeclarationClassesRegistryTypedFieldErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryFieldValueErrorCode {
  VALUE_OUTSIDE_CONSTRUCTOR,
  START_VALUE_NOT_LITERAL,
};

struct DeclarationClassesRegistryFieldValueError {
  DeclarationClassesRegistryFieldValueErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryTypeSeenErrorCode {
  STATIC_VALUE_NOT_LITERAL,
  FUNCTION_FIELD_UNTYPED,
};

struct DeclarationClassesRegistryTypeSeenError {
  DeclarationClassesRegistryTypeSeenErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryStaticTypeParamsErrorCode {
  USES_TYPE_PARAMS,
};

struct DeclarationClassesRegistryStaticTypeParamsError {
  DeclarationClassesRegistryStaticTypeParamsErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryStaticValueErrorCode {
  NO_VALUE,
};

struct DeclarationClassesRegistryStaticValueError {
  DeclarationClassesRegistryStaticValueErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryParamTypeErrorCode {
  MORE_ARGUMENTS,
};

struct DeclarationClassesRegistryParamTypeError {
  DeclarationClassesRegistryParamTypeErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryAbstractReturnErrorCode {
  NO_RETURN_TYPE,
};

struct DeclarationClassesRegistryAbstractReturnError {
  DeclarationClassesRegistryAbstractReturnErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryGenericMethodErrorCode {
  ABSTRACT,
  OVERRIDE,
};

struct DeclarationClassesRegistryGenericMethodError {
  DeclarationClassesRegistryGenericMethodErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryVariantPairErrorCode {
  ONLY_RETURN_DIFFERS,
  SAME_PARAMETERS,
  SAME_FOR_COUNT,
};

struct DeclarationClassesRegistryVariantPairError {
  DeclarationClassesRegistryVariantPairErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryParentMethodErrorCode {
  PARENT_METHOD,
};

struct DeclarationClassesRegistryParentMethodError {
  DeclarationClassesRegistryParentMethodErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryNarrowedFieldErrorCode {
  OTHER_LAYOUT,
};

struct DeclarationClassesRegistryNarrowedFieldError {
  DeclarationClassesRegistryNarrowedFieldErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryBaseClassErrorCode {
  NOT_NAMED,
  ALREADY_REPORTED,
  EXTENDS_ITSELF,
  RUNTIME_CLASS,
  VALUE_BASE,
  ERROR_CLASS,
  NOT_A_CLASS,
  NO_SUCH_CLASS,
};

struct DeclarationClassesRegistryBaseClassError {
  DeclarationClassesRegistryBaseClassErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryImplementedTypeErrorCode {
  CLASS_AS_INTERFACE,
  NOT_AN_INTERFACE,
};

struct DeclarationClassesRegistryImplementedTypeError {
  DeclarationClassesRegistryImplementedTypeErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryOverrideTargetErrorCode {
  NO_PARENT,
  NO_FIELD,
};

struct DeclarationClassesRegistryOverrideTargetError {
  DeclarationClassesRegistryOverrideTargetErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryInheritedVisibleErrorCode {
  PRIVATE_IN_BASE,
};

struct DeclarationClassesRegistryInheritedVisibleError {
  DeclarationClassesRegistryInheritedVisibleErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryAccessKeptErrorCode {
  NARROWER_ACCESS,
};

struct DeclarationClassesRegistryAccessKeptError {
  DeclarationClassesRegistryAccessKeptErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryValueGivenErrorCode {
  NO_VALUE,
};

struct DeclarationClassesRegistryValueGivenError {
  DeclarationClassesRegistryValueGivenErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryNotSelfErrorCode {
  EXTENDS_ITSELF,
};

struct DeclarationClassesRegistryNotSelfError {
  DeclarationClassesRegistryNotSelfErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryInterfaceBaseErrorCode {
  CLASS_BASE,
  NOT_AN_INTERFACE,
};

struct DeclarationClassesRegistryInterfaceBaseError {
  DeclarationClassesRegistryInterfaceBaseErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistrySameSignaturesErrorCode {
  OVERLOAD_CONFLICT,
};

struct DeclarationClassesRegistrySameSignaturesError {
  DeclarationClassesRegistrySameSignaturesErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistrySameFieldTypeErrorCode {
  FIELD_CONFLICT,
};

struct DeclarationClassesRegistrySameFieldTypeError {
  DeclarationClassesRegistrySameFieldTypeErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryNewOverloadErrorCode {
  REDECLARED_METHOD,
};

struct DeclarationClassesRegistryNewOverloadError {
  DeclarationClassesRegistryNewOverloadErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryNewFieldErrorCode {
  REDECLARED_MEMBER,
};

struct DeclarationClassesRegistryNewFieldError {
  DeclarationClassesRegistryNewFieldErrorCode code;
  std::string message;
};

enum class DeclarationClassesRegistryNarrowsFieldErrorCode {
  WIDENED_FIELD,
};

struct DeclarationClassesRegistryNarrowsFieldError {
  DeclarationClassesRegistryNarrowsFieldErrorCode code;
  std::string message;
};

class ErrorTextUniqueField {
public:
  static std::string previous();
  static std::string duplicate(llvm::StringRef field, llvm::StringRef className);
};

class ErrorTextFieldOrMethod {
public:
  static std::string field();
  static std::string shared(llvm::StringRef field, llvm::StringRef className);
};

class ErrorTextSetLater {
public:
  static std::string definite(llvm::StringRef field);
};

class ErrorTextTypedField {
public:
  static std::string missing(llvm::StringRef field);
  static std::string property(llvm::StringRef field);
};

class ErrorTextDeclaredValue {
public:
  static std::string constructor(llvm::StringRef field);
  static std::string literal(llvm::StringRef field);
};

class ErrorTextTypeSeen {
public:
  static std::string shared(llvm::StringRef field);
  static std::string function(llvm::StringRef field);
};

class ErrorTextStaticParams {
public:
  static std::string shared(llvm::StringRef field, llvm::StringRef className);
};

class ErrorTextStaticValue {
public:
  static std::string missing(llvm::StringRef field, llvm::StringRef type);
};

class ErrorTextParamType {
public:
  static std::string extra(llvm::StringRef param, llvm::StringRef type);
};

class ErrorTextAbstractReturn {
public:
  static std::string missing(llvm::StringRef method, llvm::StringRef className);
};

class ErrorTextGenericMethod {
public:
  static std::string abstract();
  static std::string overrides();
};

class ErrorTextVariantPair {
public:
  static std::string other();
  static std::string duplicate(llvm::StringRef variant);
  static std::string returnOnly(bool constructor, llvm::StringRef name);
  static std::string ambiguous(llvm::StringRef first, llvm::StringRef second, int count);
};

class ErrorTextParentMethod {
public:
  static std::string clash(llvm::StringRef field, llvm::StringRef parent);
};

class ErrorTextNarrowedField {
public:
  static std::string layout(llvm::StringRef field, llvm::StringRef className, llvm::StringRef type,
                            llvm::StringRef owner, llvm::StringRef inherited);
};

class ErrorTextExtends {
public:
  static std::string byName();
  static std::string itself(llvm::StringRef cls, llvm::StringRef through);
  static std::string runtime(llvm::StringRef cls, llvm::StringRef parent);
  static std::string valueBase(llvm::StringRef cls, llvm::StringRef parent);
  static std::string errorClass(llvm::StringRef name, llvm::StringRef cls);
  static std::string notClass(llvm::StringRef name, llvm::StringRef cls, bool alias);
  static std::string noClass(llvm::StringRef name);
  static std::string classIface(llvm::StringRef type, bool extends);
  static std::string notIface(llvm::StringRef type);
};

class ErrorTextRedeclared {
public:
  static std::string noField(llvm::StringRef name, llvm::StringRef parent);
  static std::string noParent(llvm::StringRef name, llvm::StringRef cls);
  static std::string isPrivate(llvm::StringRef name, llvm::StringRef owner);
  static std::string narrower(llvm::StringRef name, llvm::StringRef baseAccess,
                              llvm::StringRef owner, llvm::StringRef access, llvm::StringRef cls);
  static std::string noValue(llvm::StringRef name, llvm::StringRef owner);
};

class ErrorTextInterfaceBases {
public:
  static std::string itself(llvm::StringRef alias, llvm::StringRef through);
  static std::string classBase(llvm::StringRef type);
  static std::string notIface(llvm::StringRef type);
  static std::string twoMethods(llvm::StringRef alias, llvm::StringRef name);
  static std::string twoFields(llvm::StringRef alias, llvm::StringRef name, llvm::StringRef origin,
                               llvm::StringRef type, llvm::StringRef base, llvm::StringRef other);
  static std::string method(llvm::StringRef alias, llvm::StringRef name);
  static std::string member(llvm::StringRef alias, llvm::StringRef name);
  static std::string narrowed(llvm::StringRef name, llvm::StringRef alias, llvm::StringRef type,
                              llvm::StringRef origin, llvm::StringRef other);
};

}
