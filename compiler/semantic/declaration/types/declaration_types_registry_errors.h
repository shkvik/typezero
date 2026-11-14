#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class DeclarationTypesRegistryBuiltinAliasErrorCode {
  REDEFINES_BUILTIN,
  REDEFINES_NUMBER,
  DECLARED_FOR_TSC,
};

struct DeclarationTypesRegistryBuiltinAliasError {
  DeclarationTypesRegistryBuiltinAliasErrorCode code;
  std::string message;
};

enum class DeclarationTypesRegistryFreeTypeNameErrorCode {
  DUPLICATE_TYPE,
};

struct DeclarationTypesRegistryFreeTypeNameError {
  DeclarationTypesRegistryFreeTypeNameErrorCode code;
  std::string message;
};

enum class DeclarationTypesRegistryAliasCycleErrorCode {
  REFERS_TO_ITSELF,
};

struct DeclarationTypesRegistryAliasCycleError {
  DeclarationTypesRegistryAliasCycleErrorCode code;
  std::string message;
};

enum class DeclarationTypesRegistryRecursiveUnionErrorCode {
  NOT_A_UNION,
};

struct DeclarationTypesRegistryRecursiveUnionError {
  DeclarationTypesRegistryRecursiveUnionErrorCode code;
  std::string message;
};

enum class DeclarationTypesRegistryEnumCycleErrorCode {
  DEPENDS_ON_ITSELF,
};

struct DeclarationTypesRegistryEnumCycleError {
  DeclarationTypesRegistryEnumCycleErrorCode code;
  std::string message;
};

enum class DeclarationTypesRegistryUniqueMemberErrorCode {
  DUPLICATE_MEMBER,
};

struct DeclarationTypesRegistryUniqueMemberError {
  DeclarationTypesRegistryUniqueMemberErrorCode code;
  std::string message;
};

enum class DeclarationTypesRegistryMemberConstantErrorCode {
  NOT_CONSTANT,
};

struct DeclarationTypesRegistryMemberConstantError {
  DeclarationTypesRegistryMemberConstantErrorCode code;
  std::string message;
};

enum class DeclarationTypesRegistryCountedMemberErrorCode {
  NO_VALUE,
};

struct DeclarationTypesRegistryCountedMemberError {
  DeclarationTypesRegistryCountedMemberErrorCode code;
  std::string message;
};

enum class DeclarationTypesRegistryWholeMemberErrorCode {
  NOT_WHOLE,
};

struct DeclarationTypesRegistryWholeMemberError {
  DeclarationTypesRegistryWholeMemberErrorCode code;
  std::string message;
};

enum class DeclarationTypesRegistryEnumKindsErrorCode {
  MIXED,
};

struct DeclarationTypesRegistryEnumKindsError {
  DeclarationTypesRegistryEnumKindsErrorCode code;
  std::string message;
};

class ErrorTextBuiltinAlias {
public:
  static std::string builtin(llvm::StringRef name);
  static std::string number(llvm::StringRef name);
};

class ErrorTextFreeTypeName {
public:
  static std::string previous();
  static std::string duplicate(llvm::StringRef name);
};

class ErrorTextAliasCycle {
public:
  static std::string itself(llvm::StringRef name);
};

class ErrorTextRecursiveUnion {
public:
  static std::string notUnion(llvm::StringRef name);
};

class ErrorTextEnumCycle {
public:
  static std::string itself(llvm::StringRef name);
};

class ErrorTextUniqueMember {
public:
  static std::string duplicate(llvm::StringRef name);
};

class ErrorTextMemberConstant {
public:
  static std::string notConst();
};

class ErrorTextCountedMember {
public:
  static std::string noValue(llvm::StringRef name);
};

class ErrorTextWholeMember {
public:
  static std::string notWhole(llvm::StringRef name, llvm::StringRef value);
};

class ErrorTextEnumKinds {
public:
  static std::string mixed(llvm::StringRef name);
};

}
