#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LibraryResultRegistryOkErrTypeArgsErrorCode {
  NOT_TWO_TYPE_ARGUMENTS,
};

struct LibraryResultRegistryOkErrTypeArgsError {
  LibraryResultRegistryOkErrTypeArgsErrorCode code;
  std::string message;
};

enum class LibraryResultRegistryStaticMethodErrorCode {
  NO_SUCH_STATIC_METHOD,
};

struct LibraryResultRegistryStaticMethodError {
  LibraryResultRegistryStaticMethodErrorCode code;
  std::string message;
};

enum class LibraryResultRegistryUsedValueErrorCode {
  PROMISE_DROPPED,
  RESULTS_UNUSED,
  CALLBACK_RESULT_DROPPED,
  RESULT_UNUSED,
};

struct LibraryResultRegistryUsedValueError {
  LibraryResultRegistryUsedValueErrorCode code;
  std::string message;
};

class ErrorTextOkErr {
public:
  static std::string typeArgs(llvm::StringRef name);
  static std::string voidValue(llvm::StringRef name);
};

class ErrorTextCombine {
public:
  static std::string noStatic(llvm::StringRef name, llvm::StringRef method);
  static std::string holds(llvm::StringRef what, bool values, llvm::StringRef type);
  static std::string notResult(llvm::StringRef what, llvm::StringRef type);
  static std::string noUnion(llvm::StringRef what, const Type *before, const Type *error);
  static std::string notArray(llvm::StringRef what, llvm::StringRef type);
};

class ErrorTextResultMember {
public:
  static std::string value(llvm::StringRef r);
  static std::string error(llvm::StringRef r);
  static std::string method(llvm::StringRef name, llvm::StringRef r);
  static std::string noMember(llvm::StringRef name, llvm::StringRef type);
};

class ErrorTextUnusedResult {
public:
  static std::string results();
  static std::string callback();
  static std::string result();
};

class ErrorTextResultUnion {
public:
  static std::string why(const Type *a, const Type *b);
};

class ErrorTextResultMethod {
public:
  static std::string noMember(llvm::StringRef name, llvm::StringRef type);
  static std::string unwrapOr(llvm::StringRef value, llvm::StringRef other);
  static std::string function(llvm::StringRef name, llvm::StringRef type);
  static std::string match(llvm::StringRef a, llvm::StringRef b);
  static std::string result(llvm::StringRef name, llvm::StringRef type, bool andThen);
  static std::string errors(const Type *a, llvm::StringRef name, const Type *b);
  static std::string values(const Type *value, const Type *other);
};

enum class LibraryResultRegistryResultTypeArgsErrorCode {
  NOT_TWO_TYPE_ARGUMENTS,
};

struct LibraryResultRegistryResultTypeArgsError {
  LibraryResultRegistryResultTypeArgsErrorCode code;
  std::string message;
};

class ErrorTextResultType {
public:
  static std::string typeArgs(llvm::StringRef name);
};

}
