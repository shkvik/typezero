#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeUnionsRegistryCheckUnionMemberErrorCode {
  TAGGED_UNION,
  NOT_ACCESSIBLE,
  NO_SUCH_FIELD,
  MAY_LACK_FIELD,
  FIELD_TYPES_CLASH,
};

struct CodeUnionsRegistryCheckUnionMemberError {
  CodeUnionsRegistryCheckUnionMemberErrorCode code;
  std::string message;
};

enum class CodeUnionsRegistryCheckUnionFieldTargetErrorCode {
  MAY_LACK_FIELD,
  NOT_ACCESSIBLE,
  READ_ONLY_FIELD,
  FIELD_TYPES_DIFFER,
};

struct CodeUnionsRegistryCheckUnionFieldTargetError {
  CodeUnionsRegistryCheckUnionFieldTargetErrorCode code;
  std::string message;
};

enum class CodeUnionsRegistryCheckUnionMethodCallErrorCode {
  NOT_NARROWED,
  NOT_ACCESSIBLE,
  SEVERAL_VARIANTS,
  INTERFACE_MEMBER,
  NOT_A_METHOD,
  METHOD_TYPES_DIFFER,
};

struct CodeUnionsRegistryCheckUnionMethodCallError {
  CodeUnionsRegistryCheckUnionMethodCallErrorCode code;
  std::string message;
};

class ErrorTextUnionMember {
public:
  static std::string narrow(llvm::StringRef name, llvm::StringRef type, llvm::StringRef typeOf);
  static std::string noField(llvm::StringRef type, llvm::StringRef name);
  static std::string mayLack(llvm::StringRef name, llvm::StringRef lacking, llvm::StringRef type,
                             llvm::StringRef hint);
  static std::string tagHint(llvm::StringRef field, llvm::StringRef tag);
  static std::string classHint(llvm::StringRef cls);
  static std::string inHint(llvm::StringRef name);
  static std::string clash(llvm::StringRef type, llvm::StringRef name, llvm::StringRef problem);
};

class ErrorTextUnionTarget {
public:
  static std::string readOnly(llvm::StringRef name, llvm::StringRef member);
  static std::string differ(llvm::StringRef type, llvm::StringRef name, llvm::StringRef first,
                            llvm::StringRef second);
};

class ErrorTextUnionMethod {
public:
  static std::string variants(llvm::StringRef name, llvm::StringRef member,
                              llvm::StringRef receiver);
  static std::string isBase(llvm::StringRef name, llvm::StringRef receiver, llvm::StringRef member);
  static std::string notMethod(llvm::StringRef receiver, llvm::StringRef member,
                               llvm::StringRef name, bool field);
  static std::string differ(llvm::StringRef receiver, llvm::StringRef name, llvm::StringRef first,
                            llvm::StringRef second);
};

}
