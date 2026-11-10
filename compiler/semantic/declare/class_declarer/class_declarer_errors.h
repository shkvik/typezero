#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ClassDeclarerUniqueFieldErrorCode {
  DUPLICATE_FIELD,
};

struct ClassDeclarerUniqueFieldError {
  ClassDeclarerUniqueFieldErrorCode code;
  std::string message;
};

class ErrorTextUniqueField {
public:
  static std::string previous();
  static std::string duplicate(llvm::StringRef field, llvm::StringRef className);
};

enum class ClassDeclarerFieldOrMethodErrorCode {
  FIELD_AND_METHOD,
};

struct ClassDeclarerFieldOrMethodError {
  ClassDeclarerFieldOrMethodErrorCode code;
  std::string message;
};

class ErrorTextFieldOrMethod {
public:
  static std::string field();
  static std::string shared(llvm::StringRef field, llvm::StringRef className);
};

enum class ClassDeclarerSetLaterErrorCode {
  SET_LATER,
};

struct ClassDeclarerSetLaterError {
  ClassDeclarerSetLaterErrorCode code;
  std::string message;
};

class ErrorTextSetLater {
public:
  static std::string definite(llvm::StringRef field);
};

enum class ClassDeclarerTypedFieldErrorCode {
  NO_TYPE,
  PROPERTY_NO_TYPE,
};

struct ClassDeclarerTypedFieldError {
  ClassDeclarerTypedFieldErrorCode code;
  std::string message;
};

class ErrorTextTypedField {
public:
  static std::string missing(llvm::StringRef field);
  static std::string property(llvm::StringRef field);
};

enum class ClassDeclarerFieldValueErrorCode {
  VALUE_OUTSIDE_CONSTRUCTOR,
};

struct ClassDeclarerFieldValueError {
  ClassDeclarerFieldValueErrorCode code;
  std::string message;
};

class ErrorTextDeclaredValue {
public:
  static std::string constructor(llvm::StringRef field);
};

enum class ClassDeclarerStaticTypeParamsErrorCode {
  USES_TYPE_PARAMS,
};

struct ClassDeclarerStaticTypeParamsError {
  ClassDeclarerStaticTypeParamsErrorCode code;
  std::string message;
};

class ErrorTextStaticParams {
public:
  static std::string shared(llvm::StringRef field, llvm::StringRef className);
};

enum class ClassDeclarerStaticValueErrorCode {
  NO_VALUE,
};

struct ClassDeclarerStaticValueError {
  ClassDeclarerStaticValueErrorCode code;
  std::string message;
};

class ErrorTextStaticValue {
public:
  static std::string missing(llvm::StringRef field, llvm::StringRef type);
};

enum class ClassDeclarerParamTypeErrorCode {
  MORE_ARGUMENTS,
};

struct ClassDeclarerParamTypeError {
  ClassDeclarerParamTypeErrorCode code;
  std::string message;
};

class ErrorTextParamType {
public:
  static std::string extra(llvm::StringRef param, llvm::StringRef type);
};

enum class ClassDeclarerAbstractReturnErrorCode {
  NO_RETURN_TYPE,
};

struct ClassDeclarerAbstractReturnError {
  ClassDeclarerAbstractReturnErrorCode code;
  std::string message;
};

class ErrorTextAbstractReturn {
public:
  static std::string missing(llvm::StringRef method, llvm::StringRef className);
};

enum class ClassDeclarerGenericMethodErrorCode {
  ABSTRACT,
  OVERRIDE,
};

struct ClassDeclarerGenericMethodError {
  ClassDeclarerGenericMethodErrorCode code;
  std::string message;
};

class ErrorTextGenericMethod {
public:
  static std::string abstract();
  static std::string overrides();
};

enum class ClassDeclarerVariantPairErrorCode {
  ONLY_RETURN_DIFFERS,
  SAME_PARAMETERS,
  SAME_FOR_COUNT,
};

struct ClassDeclarerVariantPairError {
  ClassDeclarerVariantPairErrorCode code;
  std::string message;
};

class ErrorTextVariantPair {
public:
  static std::string other();
  static std::string duplicate(llvm::StringRef variant);
  static std::string returnOnly(bool constructor, llvm::StringRef name);
  static std::string ambiguous(llvm::StringRef first, llvm::StringRef second, int count);
};

enum class ClassDeclarerParentMethodErrorCode {
  PARENT_METHOD,
};

struct ClassDeclarerParentMethodError {
  ClassDeclarerParentMethodErrorCode code;
  std::string message;
};

class ErrorTextParentMethod {
public:
  static std::string clash(llvm::StringRef field, llvm::StringRef parent);
};

enum class ClassDeclarerNarrowedFieldErrorCode {
  OTHER_LAYOUT,
};

struct ClassDeclarerNarrowedFieldError {
  ClassDeclarerNarrowedFieldErrorCode code;
  std::string message;
};

class ErrorTextNarrowedField {
public:
  static std::string layout(llvm::StringRef field, llvm::StringRef className, llvm::StringRef type,
                            llvm::StringRef owner, llvm::StringRef inherited);
};

}
