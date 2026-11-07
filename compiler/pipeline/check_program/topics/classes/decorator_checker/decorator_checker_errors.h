#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class DecoratorCheckerNamesDistinctErrorCode {
  SAME_NAME,
};

struct DecoratorCheckerNamesDistinctError {
  DecoratorCheckerNamesDistinctErrorCode code;
  std::string message;
};

class ErrorTextDecoratedNames {
public:
  static std::string shared(llvm::StringRef what, llvm::StringRef cls, llvm::StringRef records);
  static std::string bothKinds(llvm::StringRef name);
  static std::string variants(llvm::StringRef name);
  static std::string design();
  static std::string recordsOf(llvm::StringRef shared);
  static std::string other();
  static std::string sameRecord(llvm::StringRef cls);
};

enum class DecoratorCheckerSecondConstructorErrorCode {
  TWO_DECORATED_CONSTRUCTORS,
};

struct DecoratorCheckerSecondConstructorError {
  DecoratorCheckerSecondConstructorErrorCode code;
  std::string message;
};

enum class DecoratorCheckerMemberDecoratableErrorCode {
  ACCESSOR_FIELD,
  ABSTRACT_METHOD,
};

struct DecoratorCheckerMemberDecoratableError {
  DecoratorCheckerMemberDecoratableErrorCode code;
  std::string message;
};

class ErrorTextDecorate {
public:
  static std::string accessor();
  static std::string abstract();
  static std::string arguments(llvm::StringRef label, llvm::StringRef place, llvm::StringRef needs,
                               llvm::StringRef given, llvm::StringRef args);
  static std::string descriptor(llvm::StringRef label);
  static std::string position(llvm::StringRef label);
  static std::string generic(llvm::StringRef cls);
  static std::string metadata();
};

enum class DecoratorCheckerDecoratorFitsErrorCode {
  TOO_MANY_ARGUMENTS,
  DESCRIPTOR_FOR_PARAMETER,
  POSITION_FOR_METHOD,
};

struct DecoratorCheckerDecoratorFitsError {
  DecoratorCheckerDecoratorFitsErrorCode code;
  std::string message;
};

enum class DecoratorCheckerGenericDescriptorErrorCode {
  GENERIC_CLASS_DESCRIPTOR,
};

struct DecoratorCheckerGenericDescriptorError {
  DecoratorCheckerGenericDescriptorErrorCode code;
  std::string message;
};

enum class DecoratorCheckerMetadataArgsErrorCode {
  WRONG_METADATA_ARGUMENTS,
};

struct DecoratorCheckerMetadataArgsError {
  DecoratorCheckerMetadataArgsErrorCode code;
  std::string message;
};

enum class DecoratorCheckerDecoratorResultErrorCode {
  RUNTIME_CLASS,
  CLASS_RESULT,
  METHOD_RESULT,
  OTHER_RESULT,
};

struct DecoratorCheckerDecoratorResultError {
  DecoratorCheckerDecoratorResultErrorCode code;
  std::string message;
};

class ErrorTextDecoratorResult {
public:
  static std::string runtime(llvm::StringRef label, llvm::StringRef result,
                             llvm::StringRef current);
  static std::string forClass(llvm::StringRef label, llvm::StringRef result);
  static std::string forMethod(llvm::StringRef label, llvm::StringRef result);
  static std::string other(llvm::StringRef label, llvm::StringRef result, llvm::StringRef place);
};

class ErrorTextDecoratorPlace {
public:
  static std::string aClass();
  static std::string aField();
  static std::string aMethod();
  static std::string aParameter();
  static std::string classArgs();
  static std::string fieldArgs();
  static std::string methodArgs();
  static std::string paramArgs();
  static std::string label(llvm::StringRef name);
};

}
