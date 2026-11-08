#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class InheritCheckerAbstractNeedsClassErrorCode {
  ABSTRACT_IN_CONCRETE,
};

struct InheritCheckerAbstractNeedsClassError {
  InheritCheckerAbstractNeedsClassErrorCode code;
  std::string message;
};

class ErrorTextAbstract {
public:
  static std::string needsClass(llvm::StringRef fn, llvm::StringRef cls);
  static std::string notImpl(llvm::StringRef cls, llvm::StringRef variant, llvm::StringRef owner);
};

enum class InheritCheckerAbstractImplementedErrorCode {
  NOT_IMPLEMENTED,
};

struct InheritCheckerAbstractImplementedError {
  InheritCheckerAbstractImplementedErrorCode code;
  std::string message;
};

enum class InheritCheckerFieldMatchesErrorCode {
  FIELD_TYPE_DIFFERS,
};

struct InheritCheckerFieldMatchesError {
  InheritCheckerFieldMatchesErrorCode code;
  std::string message;
};

class ErrorTextImplements {
public:
  static std::string prefix(llvm::StringRef cls, llvm::StringRef iface);
  static std::string fieldType(llvm::StringRef prefix, llvm::StringRef name, llvm::StringRef type,
                               llvm::StringRef cls, llvm::StringRef actual);
  static std::string noMember(llvm::StringRef prefix, llvm::StringRef name, bool optional,
                              llvm::StringRef type);
  static std::string fieldNotFn(llvm::StringRef name, llvm::StringRef type, llvm::StringRef iface,
                                llvm::StringRef cls);
  static std::string mustField(llvm::StringRef name, llvm::StringRef iface, llvm::StringRef cls);
  static std::string returns(llvm::StringRef name, llvm::StringRef ret, llvm::StringRef iface,
                             llvm::StringRef cls, llvm::StringRef says);
  static std::string params(llvm::StringRef prefix, llvm::StringRef name, llvm::StringRef type,
                            llvm::StringRef cls, llvm::StringRef variants);
  static std::string noVariant(llvm::StringRef prefix, llvm::StringRef name,
                               llvm::StringRef signature);
};

enum class InheritCheckerMemberDeclaredErrorCode {
  NO_MEMBER,
};

struct InheritCheckerMemberDeclaredError {
  InheritCheckerMemberDeclaredErrorCode code;
  std::string message;
};

enum class InheritCheckerMethodForFieldErrorCode {
  FIELD_AS_METHOD,
};

struct InheritCheckerMethodForFieldError {
  InheritCheckerMethodForFieldErrorCode code;
  std::string message;
};

enum class InheritCheckerReplaceableErrorCode {
  METHOD_NOT_FIELD,
};

struct InheritCheckerReplaceableError {
  InheritCheckerReplaceableErrorCode code;
  std::string message;
};

enum class InheritCheckerReturnMatchesErrorCode {
  RETURN_DIFFERS,
};

struct InheritCheckerReturnMatchesError {
  InheritCheckerReturnMatchesErrorCode code;
  std::string message;
};

enum class InheritCheckerVariantFoundErrorCode {
  OTHER_PARAMETERS,
};

struct InheritCheckerVariantFoundError {
  InheritCheckerVariantFoundErrorCode code;
  std::string message;
};

enum class InheritCheckerSignatureVariantErrorCode {
  NO_VARIANT,
};

struct InheritCheckerSignatureVariantError {
  InheritCheckerSignatureVariantErrorCode code;
  std::string message;
};

enum class InheritCheckerSuperAllowedErrorCode {
  NOT_IN_CONSTRUCTOR,
};

struct InheritCheckerSuperAllowedError {
  InheritCheckerSuperAllowedErrorCode code;
  std::string message;
};

class ErrorTextSuperCall {
public:
  static std::string outside();
  static std::string noParent(llvm::StringRef cls);
  static std::string statement();
  static std::string twice(llvm::StringRef parent);
  static std::string noArgs(llvm::StringRef parent);
};

enum class InheritCheckerParentClassErrorCode {
  NO_PARENT,
  BASE_ONLY,
};

struct InheritCheckerParentClassError {
  InheritCheckerParentClassErrorCode code;
  std::string message;
};

enum class InheritCheckerRootSuperErrorCode {
  NOT_A_STATEMENT,
};

struct InheritCheckerRootSuperError {
  InheritCheckerRootSuperErrorCode code;
  std::string message;
};

enum class InheritCheckerSuperOnceErrorCode {
  CALLED_TWICE,
};

struct InheritCheckerSuperOnceError {
  InheritCheckerSuperOnceErrorCode code;
  std::string message;
};

enum class InheritCheckerSuperArgumentsErrorCode {
  NO_CONSTRUCTOR,
};

struct InheritCheckerSuperArgumentsError {
  InheritCheckerSuperArgumentsErrorCode code;
  std::string message;
};

enum class InheritCheckerSuperContextErrorCode {
  NO_SUPER_CLASS,
  BASE_ONLY,
};

struct InheritCheckerSuperContextError {
  InheritCheckerSuperContextErrorCode code;
  std::string message;
};

class ErrorTextSuperMethod {
public:
  static std::string outside();
  static std::string early(llvm::StringRef name, llvm::StringRef field);
  static std::string isField(llvm::StringRef name, llvm::StringRef parent);
  static std::string noMethod(llvm::StringRef parent, llvm::StringRef name);
  static std::string abstract(llvm::StringRef name, llvm::StringRef owner);
};

enum class InheritCheckerObjectCompleteErrorCode {
  CALLED_BEFORE_SET,
};

struct InheritCheckerObjectCompleteError {
  InheritCheckerObjectCompleteErrorCode code;
  std::string message;
};

enum class InheritCheckerParentMethodsErrorCode {
  FIELD_OF_PARENT,
  NO_SUCH_METHOD,
};

struct InheritCheckerParentMethodsError {
  InheritCheckerParentMethodsErrorCode code;
  std::string message;
};

enum class InheritCheckerMethodBodyErrorCode {
  ABSTRACT_METHOD,
};

struct InheritCheckerMethodBodyError {
  InheritCheckerMethodBodyErrorCode code;
  std::string message;
};

}
