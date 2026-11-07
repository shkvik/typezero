#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TypeDeclarerBuiltinAliasErrorCode {
  REDEFINES_BUILTIN,
  REDEFINES_NUMBER,
  DECLARED_FOR_TSC,
};

struct TypeDeclarerBuiltinAliasError {
  TypeDeclarerBuiltinAliasErrorCode code;
  std::string message;
};

class ErrorTextBuiltinAlias {
public:
  static std::string builtin(llvm::StringRef name);
  static std::string number(llvm::StringRef name);
};

enum class TypeDeclarerFreeTypeNameErrorCode {
  DUPLICATE_TYPE,
};

struct TypeDeclarerFreeTypeNameError {
  TypeDeclarerFreeTypeNameErrorCode code;
  std::string message;
};

class ErrorTextFreeTypeName {
public:
  static std::string previous();
  static std::string duplicate(llvm::StringRef name);
};

enum class TypeDeclarerAliasCycleErrorCode {
  REFERS_TO_ITSELF,
};

struct TypeDeclarerAliasCycleError {
  TypeDeclarerAliasCycleErrorCode code;
  std::string message;
};

class ErrorTextAliasCycle {
public:
  static std::string itself(llvm::StringRef name);
};

enum class TypeDeclarerRecursiveUnionErrorCode {
  NOT_A_UNION,
};

struct TypeDeclarerRecursiveUnionError {
  TypeDeclarerRecursiveUnionErrorCode code;
  std::string message;
};

class ErrorTextRecursiveUnion {
public:
  static std::string notUnion(llvm::StringRef name);
};

enum class TypeDeclarerEnumCycleErrorCode {
  DEPENDS_ON_ITSELF,
};

struct TypeDeclarerEnumCycleError {
  TypeDeclarerEnumCycleErrorCode code;
  std::string message;
};

class ErrorTextEnumCycle {
public:
  static std::string itself(llvm::StringRef name);
};

enum class TypeDeclarerUniqueMemberErrorCode {
  DUPLICATE_MEMBER,
};

struct TypeDeclarerUniqueMemberError {
  TypeDeclarerUniqueMemberErrorCode code;
  std::string message;
};

class ErrorTextUniqueMember {
public:
  static std::string duplicate(llvm::StringRef name);
};

enum class TypeDeclarerMemberConstantErrorCode {
  NOT_CONSTANT,
};

struct TypeDeclarerMemberConstantError {
  TypeDeclarerMemberConstantErrorCode code;
  std::string message;
};

class ErrorTextMemberConstant {
public:
  static std::string notConst();
};

enum class TypeDeclarerCountedMemberErrorCode {
  NO_VALUE,
};

struct TypeDeclarerCountedMemberError {
  TypeDeclarerCountedMemberErrorCode code;
  std::string message;
};

class ErrorTextCountedMember {
public:
  static std::string noValue(llvm::StringRef name);
};

enum class TypeDeclarerWholeMemberErrorCode {
  NOT_WHOLE,
};

struct TypeDeclarerWholeMemberError {
  TypeDeclarerWholeMemberErrorCode code;
  std::string message;
};

class ErrorTextWholeMember {
public:
  static std::string notWhole(llvm::StringRef name, llvm::StringRef value);
};

enum class TypeDeclarerEnumKindsErrorCode {
  MIXED,
};

struct TypeDeclarerEnumKindsError {
  TypeDeclarerEnumKindsErrorCode code;
  std::string message;
};

class ErrorTextEnumKinds {
public:
  static std::string mixed(llvm::StringRef name);
};

enum class TypeDeclarerCheckEnumMemberErrorCode {
  NO_SUCH_MEMBER,
};

struct TypeDeclarerCheckEnumMemberError {
  TypeDeclarerCheckEnumMemberErrorCode code;
  std::string message;
};

class ErrorTextEnumMember {
public:
  static std::string missing(llvm::StringRef property, llvm::StringRef name);
};

enum class TypeDeclarerCheckEnumIndexErrorCode {
  ALREADY_REPORTED,
  STRING_ENUM,
  NOT_AN_INTEGER,
};

struct TypeDeclarerCheckEnumIndexError {
  TypeDeclarerCheckEnumIndexErrorCode code;
  std::string message;
};

class ErrorTextEnumIndex {
public:
  static std::string strings(llvm::StringRef name);
  static std::string notInteger(llvm::StringRef type);
};

}
